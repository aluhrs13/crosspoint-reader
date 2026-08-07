#pragma once

#include <string>
#include <vector>

#include "ArticleAssembler.h"
#include "ReadwiseApi.h"
#include "ReadwiseCodec.h"
#include "ReadwiseFileStore.h"
#include "ReadwiseJournal.h"

// The recoverable sync pipeline.
//
// Ordering, from issue #3: coalesce, push, pull, rebuild indexes, commit. The
// checkpoint is written last and atomically, so it is the commit point -- the
// same discipline as Section.cpp, which patches its version byte last.
//
// Every failure path leaves the previous checkpoint and the pending queue
// intact. A sync that gives up is recoverable; one that half-commits is not.

namespace readwise {

// Per-location sync policy. `feed` is an RSS firehose large enough to hit the
// API's 10,000 `count` cap on its own, and the API offers no server-side unread
// filter -- so feed syncs unread-only (seen docs are skipped during the pull and
// dropped at merge), is hard-bounded in pages walked, and is excluded from the
// deletion reconcile sweep (it expires via the unread filter and its cap
// instead). `new` (the Readwise Inbox) is deliberately not synced: the library
// views are Later, Shortlist and Feed.
struct LocationPolicy {
  Location location;
  bool unreadOnly;      // skip FLAG_SEEN docs during pull; drop seen records at sync merge
  bool reconcileSweep;  // participates in the deletion sweep
  uint8_t maxPages;     // pagination bound for this location
};

// Guards against a runaway server: with a 100-document page and a 100-document
// cap, a sync should never need more than a handful of pages per location.
inline constexpr uint8_t DEFAULT_MAX_PAGES = 64;
// The feed walk never paginates past this, so the untested question of whether
// the API caps pagination at 10,000 documents stays out of reach.
inline constexpr uint8_t FEED_MAX_PAGES = 5;

inline constexpr LocationPolicy SYNCED_LOCATIONS[] = {
    {Location::Later, false, true, DEFAULT_MAX_PAGES},
    {Location::Shortlist, false, true, DEFAULT_MAX_PAGES},
    {Location::Feed, true, false, FEED_MAX_PAGES},
};

constexpr const LocationPolicy* policyFor(Location location) {
  for (const LocationPolicy& policy : SYNCED_LOCATIONS) {
    // cppcheck-suppress useStlAlgorithm  // constexpr lookup over a 3-entry table; find_if buys nothing here
    if (policy.location == location) {
      return &policy;
    }
  }
  return nullptr;
}

constexpr bool isSyncedLocation(Location location) { return policyFor(location) != nullptr; }

// Metadata cap for the non-feed locations combined. Bodies are also prefetched
// during sync (see downloadMissingBodies), so this bounds transfer volume too.
inline constexpr uint16_t DEFAULT_DOCUMENT_CAP = 100;

// Feed is additive to the document cap: at most this many unread feed items are
// kept, newest first.
inline constexpr uint16_t DEFAULT_FEED_CAP = 50;

enum class SyncStage : uint8_t {
  Idle = 0,
  Coalescing,
  Pushing,
  Pulling,
  RebuildingIndexes,
  Committing,
};

struct SyncOutcome {
  bool ok = false;
  SyncStage failedStage = SyncStage::Idle;
  ApiStatus status = ApiStatus::Ok;
  uint16_t pushed = 0;
  uint16_t pulled = 0;
  uint16_t retained = 0;
};

class ReadwiseSyncEngine {
 public:
  ReadwiseSyncEngine(ReadwiseApi& api, ReadwiseFileStore& store, std::string baseDir);

  // Runs the five stages. On any failure the pass is abandoned with the previous
  // checkpoint and journal preserved.
  SyncOutcome sync();

  // Enumerates the synced locations and expires local documents absent from the
  // result. Separate from sync() because the API emits no deletion tombstones:
  // a deleted document simply vanishes from an updatedAfter window, so the only
  // way to observe a deletion is a full sweep.
  //
  // Expiry happens ONLY if every page of every synced location was fetched
  // successfully -- a document missing because the network failed is
  // indistinguishable from one that was deleted.
  SyncOutcome reconcile();

  // Queues a local action. These are the only two operations the API honours.
  bool queueLocationChange(const char* id, Location location, const char* remoteRev);
  bool queueSeen(const char* id, const char* remoteRev);

  // Reads one page of a location index without loading the rest. `out` is
  // cleared and filled with at most `count` documents.
  bool readIndexPage(Location location, uint16_t offset, uint16_t count, std::vector<Document>& out);

  // Rewrites docs.bin and the indexes with the queued journal overrides applied,
  // without any network traffic. This is what makes a queued action visible
  // immediately: archiving a document offline removes it from the synced
  // indexes right away rather than at the next sync.
  bool rebuildLocal();

  // Linear scan of docs.bin for one document. Used when a managed body path is
  // reopened (e.g. resume after restart) and the UI needs its metadata back.
  bool findDocument(const char* id, Document& out);

  // Downloads the body of every cached document that lacks one, so that after
  // a sync the entire library reads offline. Runs after the metadata stages;
  // failures are per-document (the on-demand path in the library remains the
  // retry), except auth/credential failures which abort the pass.
  //
  // Body fetches hit the list endpoint's 20 req/min limit, so a large first
  // sync WILL be throttled: on RateLimited the engine calls `hooks.sleepMs`
  // with the server's retry-after and retries that document. C function
  // pointers rather than std::function, per the library-code rule.
  struct BodySyncHooks {
    void* ctx = nullptr;
    // Progress after each document (done includes failures).
    void (*onProgress)(void* ctx, uint16_t done, uint16_t total) = nullptr;
    // Blocking wait; the activity supplies delay(). Never called with more
    // than RATE_LIMIT_WAIT_CAP_MS.
    void (*sleepMs)(void* ctx, uint32_t ms) = nullptr;
    // Supplies image bytes during assembly. Optional: without it articles are
    // still built and read correctly, their images degrading to alt text.
    // Injected rather than constructed here so the engine stays free of
    // network headers and host-testable.
    ArticleImageFetcher* imageFetcher = nullptr;
  };
  struct BodySyncOutcome {
    bool ok = false;
    ApiStatus status = ApiStatus::Ok;
    uint16_t downloaded = 0;
    uint16_t failed = 0;
    uint16_t total = 0;
  };
  BodySyncOutcome downloadMissingBodies(const BodySyncHooks& hooks);

  // Marks a document's body as cached (or not) by patching the flags byte of
  // its record in place. Without this, a downloaded article would read as
  // "not downloaded" after the post-download restart and be fetched again.
  // In-place rather than a full rewrite: the flags byte is the last byte of a
  // fixed-position record, so a torn write costs at worst one redundant
  // re-download, while a full ~70 KB docs.bin rewrite per article download
  // would be real SD wear.
  bool setBodyCached(const char* id, bool cached);

  // Total entries in one location index, for list sizing. Returns 0 when the
  // index is absent.
  uint16_t indexCount(Location location);

  bool loadCheckpoint(Checkpoint& out);
  ReadwiseJournal& journal() { return journal_; }

  void setDocumentCap(uint16_t cap) { documentCap_ = cap; }
  void setFeedCap(uint16_t cap) { feedCap_ = cap; }

  std::string docsPath() const { return baseDir_ + "/docs.bin"; }
  std::string journalPath() const { return baseDir_ + "/journal.bin"; }
  std::string checkpointPath() const { return baseDir_ + "/checkpoint.bin"; }
  std::string indexPath(Location location) const;
  // An article is a directory, not a file: the archive plus the Epub cache the
  // reader builds beside it (sections, extracted images, pixel caches, cover).
  // Keeping them together makes eviction one recursive delete.
  std::string articleDir(const char* id) const;
  std::string bodyPath(const char* id) const;

 private:
  // A document staged during the pull, held only as an id plus its offset in the
  // staging file. 100 documents costs ~3 KB, which is affordable; the records
  // themselves stay on disk.
  struct StagedRef {
    char id[ID_CAP];
    uint32_t offset;
    uint32_t length;
  };

  // What the index rebuild needs, gathered while docs.bin is written so the file
  // is not read back a second time.
  struct IndexEntry {
    uint16_t recordIndex;
    Location location;
    char lastMovedAt[TIMESTAMP_CAP];
  };

  ApiStatus pullToStaging(const Checkpoint& checkpoint, std::vector<StagedRef>& staged, char* highestUpdatedAt);
  // Rewrites docs.bin from `sourcePath`, whose records `refs` locates. When
  // `carryOverExisting` is set, documents already in docs.bin that `refs` does
  // not supersede are appended until the cap is reached -- that is the sync
  // path. Reconciliation passes false, because its refs already describe the
  // complete surviving set.
  //
  // `dropExpired` removes records (and bodies) the sync policy says must go:
  // `new`-location leftovers and seen documents in unread-only locations.
  // sync() and reconcile() pass true; rebuildLocal() passes false so a feed
  // article read on the device stays openable until the next sync.
  bool mergeIntoDocs(const std::string& sourcePath, const std::vector<StagedRef>& refs, bool carryOverExisting,
                     bool dropExpired, std::vector<IndexEntry>& indexEntries, uint16_t& retained);
  bool writeIndexes(std::vector<IndexEntry>& indexEntries);
  bool commitCheckpoint(const char* updatedAfter, uint16_t docCount);
  void applyQueuedOverrides(Document& doc) const;

  std::string stagingPath() const { return baseDir_ + "/incoming.bin"; }

  ReadwiseApi& api_;
  ReadwiseFileStore& store_;
  std::string baseDir_;
  ReadwiseJournal journal_;
  uint16_t documentCap_ = DEFAULT_DOCUMENT_CAP;
  uint16_t feedCap_ = DEFAULT_FEED_CAP;

  // Reused across the streaming loops rather than constructed per document: a
  // Document is ~800 bytes, well over the project's 256-byte stack guidance.
  Document scratchDoc_;
  uint8_t recordBuffer_[MAX_ENCODED_RECORD];
};

}  // namespace readwise
