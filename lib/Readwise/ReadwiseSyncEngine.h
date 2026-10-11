#pragma once

#include <string>
#include <vector>

#include "ArticleAssembler.h"
#include "ReadwiseApi.h"
#include "ReadwiseCodec.h"
#include "ReadwiseFileStore.h"
#include "ReadwiseHighlight.h"
#include "ReadwiseJournal.h"

// The recoverable sync pipeline.
//
// Ordering, from issue #3: coalesce, push, pull, rebuild indexes, commit. The
// checkpoint is written last and atomically, so it is the commit point -- the
// same discipline as Section.cpp, which patches its version byte last.
//
// Every failure path leaves the previous checkpoint and the pending queue
// intact. A sync that gives up is recoverable; one that half-commits is not.
// A 404 on one queued update is the exception: the caller can drop that op
// and the sync continues with the rest.

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
  // Quotes accepted this pass, and quotes the server rejected (4xx). A
  // rejection does not fail the sync. Both stay 0 when nothing was queued.
  uint16_t highlightsSent = 0;
  uint16_t highlightsFailed = 0;
  // What failed, for the screen and the log. Empty on success.
  // "delete <id>: HTTP 400 {...}", "highlight <id>", "pull later".
  // highlightDetail holds the first rejection ("highlight <id>: HTTP 400 ...").
  static constexpr size_t DETAIL_CAP = 160;
  char highlightDetail[DETAIL_CAP] = {};
  char detail[DETAIL_CAP] = {};
};

// Per-category totals. `named` is indexed by Category for the dense prefix
// Article..Epub. Unknown, and any value appended later, accumulates in `other`.
struct CategoryCounts {
  uint16_t total = 0;
  uint16_t unread = 0;
  uint16_t onDevice = 0;
};

struct LibraryCounts {
  static constexpr int kNamedCategories = 9;
  CategoryCounts named[kNamedCategories]{};
  CategoryCounts other{};
};

class ReadwiseSyncEngine {
 public:
  ReadwiseSyncEngine(ReadwiseApi& api, ReadwiseFileStore& store, std::string baseDir);

  // Runs the five stages. On any failure the pass is abandoned with the previous
  // checkpoint and journal preserved, except ops a NotFound hook chose to drop.
  SyncOutcome sync();

  // A queued PATCH that answers 404. The document is already gone on the
  // server (often archived or deleted). Return true to drop that op and the
  // local copy, then keep syncing. Return false to abort and leave the op
  // queued. A null hook aborts. `title` and `detail` are valid only for the
  // call; `title` is empty when the document is not cached.
  struct NotFoundHooks {
    void* ctx = nullptr;
    bool (*onNotFound)(void* ctx, const PendingOp& op, const char* title, const char* detail) = nullptr;
  };
  void setNotFoundHooks(NotFoundHooks hooks) { notFoundHooks_ = hooks; }

  // Enumerates the synced locations and expires local documents absent from the
  // result. Separate from sync() because the API emits no deletion tombstones:
  // a deleted document simply vanishes from an updatedAfter window, so the only
  // way to observe a deletion is a full sweep.
  //
  // Expiry happens ONLY if every page of every synced location was fetched
  // successfully -- a document missing because the network failed is
  // indistinguishable from one that was deleted.
  SyncOutcome reconcile();

  // Queues a local action. Location and seen are PATCH. Delete is DELETE, and
  // the local record stays until that push is accepted.
  bool queueLocationChange(const char* id, Location location, const char* remoteRev);
  bool queueSeen(const char* id, const char* remoteRev);
  bool queueDelete(const char* id, const char* remoteRev);

  // Drops the local record, cached body, and note. An unposted quote is kept
  // and still pushed on a later sync. Call only after the server has accepted
  // the delete. A failed push must leave the document in place.
  bool forgetDocument(const char* id);

  // A note kept beside the document, not in docs.bin. Empty text removes it.
  // `out` is always NUL-terminated. Returns false when there is no note.
  static constexpr size_t NOTE_CAP = 241;
  bool readNote(const char* id, char* out, size_t outCap);
  bool writeNote(const char* id, const char* text);

  // Queues a quote for the next sync. The text is cut to HIGHLIGHT_TEXT_MAX on
  // a word boundary. Title, author, and source URL are copied from docs.bin
  // when the document is still there. Empty text is refused.
  bool appendHighlight(const char* id, const char* text);

  // Quotes stored for this document, including ones already posted. `out`
  // receives at most `cap` records. Returns the count, or 0 when there are none.
  struct StoredHighlight {
    char text[HIGHLIGHT_TEXT_MAX + 1] = {};
    uint8_t flags = 0;
  };
  int readHighlights(const char* id, StoredHighlight* out, int cap);

  // Location-index offsets whose author matches, in shelf order. One document
  // is decoded at a time. `author` is the stored AUTHOR_CAP string.
  bool collectAuthorSlots(Location location, const char* author, std::vector<uint16_t>& out);

  // Loads the documents at those location-index offsets, in the order given.
  bool readIndexSlots(Location location, const uint16_t* slots, uint16_t slotCount, std::vector<Document>& out);

  // docs.bin record indexes for Later and Shortlist documents on one side of
  // `minWords`, newest `lastMovedAt` first. `minWords` itself belongs to the
  // long side. A zero word count is on neither side. An empty `author` skips
  // that test. Feed is not included.
  bool collectLengthSlots(uint32_t minWords, bool longReads, const char* author, std::vector<uint16_t>& out);

  // Loads documents by docs.bin record index, in the order given.
  bool readRecords(const uint16_t* recordIndexes, uint16_t count, std::vector<Document>& out);

  // Reads one page of a location index without loading the rest. `out` is
  // cleared and filled with at most `count` documents.
  bool readIndexPage(Location location, uint16_t offset, uint16_t count, std::vector<Document>& out);

  // Rewrites docs.bin and the indexes with the queued journal overrides applied,
  // without any network traffic. This is what makes a queued action visible
  // immediately: archiving a document offline removes it from the synced
  // indexes right away rather than at the next sync.
  bool rebuildLocal();

  // Documents with a pending archive or delete, oldest journal entry first.
  // A pending delete is `deleted` even when an archive is also queued. The
  // title falls back to the id when the document is no longer cached. At most
  // QUEUED_LIST_MAX rows; further pending ids are omitted.
  static constexpr size_t QUEUED_LIST_MAX = 64;
  struct QueuedDocument {
    char id[ID_CAP] = {};
    char title[TITLE_CAP] = {};
    char author[AUTHOR_CAP] = {};
    bool deleted = false;
  };
  bool collectQueued(std::vector<QueuedDocument>& out);
  // Drops a pending delete and a pending archive for this id. The caller
  // rebuilds the local indexes so the document returns to its shelf.
  bool undoQueued(const char* id);

  // Linear scan of docs.bin for one document. Used when a managed body path is
  // reopened (e.g. resume after restart) and the UI needs its metadata back.
  bool findDocument(const char* id, Document& out);

  // One pass over docs.bin. Queued `seen` ops count as read. `onDevice` is
  // FLAG_HAS_BODY. Returns false when docs.bin is missing or unreadable.
  bool collectLibraryCounts(LibraryCounts& out);

  // Downloads the body of every cached document that lacks one, so that after
  // a sync the entire library reads offline. Runs after the metadata stages;
  // failures are per-document (the on-demand path in the library remains the
  // retry), except auth/credential failures which abort the pass.
  //
  // Body fetches hit the list endpoint's 20 req/min limit, so a large first
  // sync WILL be throttled: on RateLimited the engine calls `hooks.sleepMs`
  // with the server's retry-after and retries that document. C function
  // pointers rather than std::function, per the library-code rule.
  //
  // One article is two long stretches: the HTML fetch (every tag is converted
  // on the way in) and then each image that conversion named. `onStep` fires
  // at the start of each stretch so the UI can say which.
  enum class BodySyncStep : uint8_t { Article = 0, Image, RateLimit };
  struct BodySyncProgress {
    uint16_t articlesDone = 0;
    uint16_t articlesTotal = 0;
    // Points at engine scratch. Valid only for the duration of the call.
    const char* title = "";
    uint32_t wordCount = 0;
    Category category = Category::Unknown;
    BodySyncStep step = BodySyncStep::Article;
    // Image index and count while `step` is Image. `index` is 0-based and
    // names the image about to be fetched.
    uint16_t index = 0;
    uint16_t count = 0;
  };
  struct BodySyncHooks {
    void* ctx = nullptr;
    // Progress after each document (done includes failures).
    void (*onProgress)(void* ctx, uint16_t done, uint16_t total) = nullptr;
    // Finer progress inside one document. Optional.
    void (*onStep)(void* ctx, const BodySyncProgress& progress) = nullptr;
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
    // Title and cause of the FIRST document that failed, so the summary can
    // name what went wrong instead of only counting it. Empty when nothing
    // failed. Only the first is kept: the summary shows one title plus a
    // count, and carrying every title would be unbounded.
    char failedTitle[TITLE_CAP] = {};
    ApiStatus failedStatus = ApiStatus::Ok;
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

  ApiStatus pullToStaging(const Checkpoint& checkpoint, std::vector<StagedRef>& staged, char* highestUpdatedAt,
                          char* step, size_t stepCap, bool refillNonFeed);
  // True when no Later or Shortlist document remains after queued overrides.
  // A missing cache counts as empty. Feed does not.
  bool nonFeedShelfEmpty();
  // Copies `step` into outcome.detail, appending the API's lastDetail when set.
  void noteFailure(SyncOutcome& outcome, ApiStatus status, const char* step);
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
  bool readIndexBounds(Location location, uint16_t& total, DocsHeader& header);
  // Fills scratchDoc_ from one location-index slot. `recordIndex` receives the
  // docs.bin record number when non-null.
  bool loadIndexedDocument(Location location, uint16_t slot, const DocsHeader& header, uint16_t* recordIndex = nullptr);
  bool loadRecord(uint16_t recordIndex, const DocsHeader& header);
  std::string notePath(const char* id) const;
  std::string highlightDir() const;
  // Current quote file. Legacy copies live under the article directory.
  std::string highlightPath(const char* id) const;
  std::string legacyHighlightPath(const char* id) const;
  std::string highlightIndexPath() const;
  // Copies a legacy quote file out of the article directory. True when the
  // current path is the one to use, including when there was nothing to move.
  bool relocateHighlight(const char* id);
  // Removes an article directory after moving any quote file out of it. If the
  // move fails the directory stays, so an unsent quote is not deleted.
  void dropArticleDir(const char* id);
  bool indexHighlight(const char* id);
  struct HighlightPushStats {
    uint16_t sent = 0;
    uint16_t failed = 0;
    char detail[SyncOutcome::DETAIL_CAP] = {};
  };
  // Posts unposted clips. A transport failure leaves the rest queued. A quote
  // the server rejects is marked and skipped. `step` receives a short label
  // ("highlight <id>", "highlight index") on a retryable failure.
  ApiStatus pushPendingHighlights(char* step, size_t stepCap, HighlightPushStats& stats);
  // One document. `stillPending` is true when a clip remains unposted.
  ApiStatus pushHighlightFile(const char* id, bool& stillPending, char* step, size_t stepCap,
                              HighlightPushStats& stats);

  std::string stagingPath() const { return baseDir_ + "/incoming.bin"; }

  ReadwiseApi& api_;
  ReadwiseFileStore& store_;
  std::string baseDir_;
  ReadwiseJournal journal_;
  NotFoundHooks notFoundHooks_;
  uint16_t documentCap_ = DEFAULT_DOCUMENT_CAP;
  uint16_t feedCap_ = DEFAULT_FEED_CAP;

  // Reused across the streaming loops rather than constructed per document: a
  // Document is ~800 bytes, well over the project's 256-byte stack guidance.
  Document scratchDoc_;
  uint8_t recordBuffer_[MAX_ENCODED_RECORD];
  // One quote while a highlights.bin record is read. The engine is heap-allocated;
  // 281 bytes does not belong on the stack.
  char highlightText_[HIGHLIGHT_TEXT_MAX + 1] = {};
  // Set for the duration of forgetDocument's rewrite. mergeIntoDocs skips it
  // and drops its body instead of carrying it forward.
  char droppingId_[ID_CAP] = {};
};

}  // namespace readwise
