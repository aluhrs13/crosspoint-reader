#pragma once

#include <string>
#include <vector>

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

// The locations synced by default. `feed` is excluded deliberately: it is an RSS
// firehose, accounted for 146 of 200 sampled documents, and is the one location
// large enough to hit the API's 10,000 `count` cap on its own.
inline constexpr Location DEFAULT_SYNCED_LOCATIONS[] = {Location::New, Location::Later};

// Metadata cap. Bodies are not prefetched -- they are fetched on open and cached
// under bodies/<id>.txt -- so this bounds only the index, not transfer volume.
inline constexpr uint16_t DEFAULT_DOCUMENT_CAP = 100;

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

  bool loadCheckpoint(Checkpoint& out);
  ReadwiseJournal& journal() { return journal_; }

  void setDocumentCap(uint16_t cap) { documentCap_ = cap; }

  std::string docsPath() const { return baseDir_ + "/docs.bin"; }
  std::string journalPath() const { return baseDir_ + "/journal.bin"; }
  std::string checkpointPath() const { return baseDir_ + "/checkpoint.bin"; }
  std::string indexPath(Location location) const;
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
  bool mergeIntoDocs(const std::string& sourcePath, const std::vector<StagedRef>& refs, bool carryOverExisting,
                     std::vector<IndexEntry>& indexEntries, uint16_t& retained);
  bool writeIndexes(std::vector<IndexEntry>& indexEntries);
  bool commitCheckpoint(const char* updatedAfter, uint16_t docCount);
  void applyQueuedOverrides(Document& doc) const;

  std::string stagingPath() const { return baseDir_ + "/incoming.bin"; }

  ReadwiseApi& api_;
  ReadwiseFileStore& store_;
  std::string baseDir_;
  ReadwiseJournal journal_;
  uint16_t documentCap_ = DEFAULT_DOCUMENT_CAP;

  // Reused across the streaming loops rather than constructed per document: a
  // Document is ~800 bytes, well over the project's 256-byte stack guidance.
  Document scratchDoc_;
  uint8_t recordBuffer_[MAX_ENCODED_RECORD];
};

}  // namespace readwise
