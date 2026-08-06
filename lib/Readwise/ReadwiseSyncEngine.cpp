#include "ReadwiseSyncEngine.h"

#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "BodyTextWriter.h"

namespace readwise {
namespace {

// The API is rate-limited to 20 list requests per minute and answers 429 with a
// retry-after of ~16 seconds. One retry is honoured; after that the pass is
// abandoned rather than looped, because a blocking wait inside an activity can
// exceed the watchdog window and reset the device.
constexpr int MAX_RATE_LIMIT_RETRIES = 1;

// Body fetches share the list endpoint's 20 req/min budget, so a throttled
// pass waits out the server's retry-after between documents. Bounded well
// under any watchdog window; the observed value is 16 s.
constexpr uint32_t DEFAULT_RATE_LIMIT_WAIT_MS = 20000;
constexpr uint32_t RATE_LIMIT_WAIT_CAP_MS = 30000;

bool sameId(const char* a, const char* b) { return strncmp(a, b, ID_CAP) == 0; }

}  // namespace

const char* apiStatusName(ApiStatus status) {
  switch (status) {
    case ApiStatus::Ok:
      return "ok";
    case ApiStatus::NoCredentials:
      return "no credentials";
    case ApiStatus::AuthFailed:
      return "auth failed";
    case ApiStatus::NetworkError:
      return "network error";
    case ApiStatus::RateLimited:
      return "rate limited";
    case ApiStatus::LowMemory:
      return "low memory";
    case ApiStatus::ParseError:
      return "parse error";
    case ApiStatus::ServerError:
      return "server error";
  }
  return "unknown";
}

ReadwiseSyncEngine::ReadwiseSyncEngine(ReadwiseApi& api, ReadwiseFileStore& store, std::string baseDir)
    : api_(api), store_(store), baseDir_(std::move(baseDir)), journal_(store, baseDir_ + "/journal.bin") {}

std::string ReadwiseSyncEngine::indexPath(Location location) const {
  return baseDir_ + "/index_" + locationName(location) + ".bin";
}

std::string ReadwiseSyncEngine::bodyPath(const char* id) const {
  return baseDir_ + "/bodies/" + (id != nullptr ? id : "") + ".txt";
}

bool ReadwiseSyncEngine::loadCheckpoint(Checkpoint& out) {
  uint8_t buffer[CHECKPOINT_SIZE];
  const int read = store_.readRange(checkpointPath(), 0, buffer, CHECKPOINT_SIZE);
  if (read != static_cast<int>(CHECKPOINT_SIZE)) {
    // No checkpoint yet means a full pull, which is the correct first-run
    // behaviour rather than an error.
    out = Checkpoint();
    return false;
  }
  return decodeCheckpoint(buffer, CHECKPOINT_SIZE, out);
}

bool ReadwiseSyncEngine::queueLocationChange(const char* id, Location location, const char* remoteRev) {
  return journal_.append(OpType::SetLocation, id, static_cast<uint8_t>(location), remoteRev);
}

bool ReadwiseSyncEngine::queueSeen(const char* id, const char* remoteRev) {
  return journal_.append(OpType::SetSeen, id, 1, remoteRev);
}

// A queued local action wins for its own field, and only that field. Everything
// else takes the incoming remote value. A queued op whose remoteRev differs from
// the incoming updated_at means the document also changed remotely; the local
// value still wins for its field, because it represents a deliberate user
// action that has not yet been pushed.
void ReadwiseSyncEngine::applyQueuedOverrides(Document& doc) const {
  if (const PendingOp* op = journal_.findLatest(doc.id, OpType::SetLocation)) {
    doc.location = static_cast<Location>(op->payload);
  }
  if (const PendingOp* op = journal_.findLatest(doc.id, OpType::SetSeen)) {
    if (op->payload != 0) {
      doc.flags |= FLAG_SEEN;
    } else {
      doc.flags &= static_cast<uint8_t>(~FLAG_SEEN);
    }
  }
}

SyncOutcome ReadwiseSyncEngine::sync() {
  SyncOutcome outcome;

  if (!store_.ensureDir(baseDir_)) {
    outcome.failedStage = SyncStage::Idle;
    return outcome;
  }

  // --- 1. Coalesce --------------------------------------------------------
  outcome.failedStage = SyncStage::Coalescing;
  if (!journal_.load()) {
    return outcome;
  }
  journal_.coalesce();

  // --- 2. Push, oldest-first ---------------------------------------------
  outcome.failedStage = SyncStage::Pushing;
  std::vector<uint32_t> acknowledged;
  acknowledged.reserve(journal_.entries().size());
  for (const PendingOp& op : journal_.entries()) {
    ApiStatus status = api_.pushOp(op);
    if (status == ApiStatus::RateLimited) {
      for (int retry = 0; retry < MAX_RATE_LIMIT_RETRIES && status == ApiStatus::RateLimited; ++retry) {
        status = api_.pushOp(op);
      }
    }
    if (status != ApiStatus::Ok) {
      // Persist what was acknowledged before giving up, so the work already
      // accepted by the server is not repeated on the next pass.
      journal_.removeAcknowledged(acknowledged);
      outcome.status = status;
      return outcome;
    }
    acknowledged.push_back(op.seq);
    ++outcome.pushed;
  }

  // --- 3. Pull ------------------------------------------------------------
  outcome.failedStage = SyncStage::Pulling;
  Checkpoint checkpoint;
  loadCheckpoint(checkpoint);

  std::vector<StagedRef> staged;
  // Sized for the records; the tombstones a pull may add on top are bounded by
  // the feed page walk but are rare enough (one per item read elsewhere since
  // the last sync) that reserving for the worst case would cost more DRAM than
  // the occasional growth.
  staged.reserve(static_cast<size_t>(documentCap_) + feedCap_);
  char highestUpdatedAt[TIMESTAMP_CAP] = {};
  copyBounded(highestUpdatedAt, TIMESTAMP_CAP, checkpoint.updatedAfter, strlen(checkpoint.updatedAfter));

  const ApiStatus pullStatus = pullToStaging(checkpoint, staged, highestUpdatedAt);
  if (pullStatus != ApiStatus::Ok) {
    store_.remove(stagingPath());
    journal_.removeAcknowledged(acknowledged);
    outcome.status = pullStatus;
    return outcome;
  }
  for (const StagedRef& ref : staged) {
    if (!ref.isTombstone()) {
      ++outcome.pulled;
    }
  }

  // --- 4. Merge and rebuild indexes --------------------------------------
  outcome.failedStage = SyncStage::RebuildingIndexes;
  std::vector<IndexEntry> indexEntries;
  indexEntries.reserve(static_cast<size_t>(documentCap_) + feedCap_);
  uint16_t retained = 0;
  if (!mergeIntoDocs(stagingPath(), staged, /*carryOverExisting=*/true, /*dropExpired=*/true, indexEntries, retained)) {
    store_.remove(stagingPath());
    journal_.removeAcknowledged(acknowledged);
    return outcome;
  }
  if (!writeIndexes(indexEntries)) {
    store_.remove(stagingPath());
    journal_.removeAcknowledged(acknowledged);
    return outcome;
  }
  outcome.retained = retained;

  // --- 5. Commit ----------------------------------------------------------
  outcome.failedStage = SyncStage::Committing;
  if (!commitCheckpoint(highestUpdatedAt, retained)) {
    journal_.removeAcknowledged(acknowledged);
    return outcome;
  }

  store_.remove(stagingPath());
  if (!journal_.removeAcknowledged(acknowledged)) {
    // The sync itself committed; failing to trim the journal only means the
    // acknowledged ops are retried next pass, which is harmless for the two
    // idempotent operations we queue.
    outcome.ok = true;
    outcome.failedStage = SyncStage::Idle;
    return outcome;
  }

  outcome.ok = true;
  outcome.failedStage = SyncStage::Idle;
  return outcome;
}

ApiStatus ReadwiseSyncEngine::pullToStaging(const Checkpoint& checkpoint, std::vector<StagedRef>& staged,
                                            char* highestUpdatedAt) {
  // A local sink so the staging writes and the id/offset bookkeeping stay
  // together; the engine's scratch buffers are reused rather than reallocated.
  struct Sink : DocumentSink {
    ReadwiseSyncEngine* engine;
    ReadwiseFileStore* store;
    std::vector<StagedRef>* staged;
    uint8_t* buffer;
    uint32_t offset = 0;
    char* highest = nullptr;
    bool failed = false;
    // Per-location policy state, set before each location's page loop. The
    // non-feed locations share one counter (the document cap spans them);
    // unread-only locations get their own counter and cap.
    bool unreadOnly = false;
    uint16_t* counter = nullptr;
    uint16_t cap = 0;

    void advanceCursor(const Document& doc) {
      // The cursor is the highest updated_at actually observed, compared
      // lexicographically -- ISO 8601 orders correctly as a string, and the
      // device has no trustworthy clock to synthesize one from.
      if (strncmp(doc.updatedAt, highest, TIMESTAMP_CAP) > 0) {
        copyBounded(highest, TIMESTAMP_CAP, doc.updatedAt, strlen(doc.updatedAt));
      }
    }

    bool onDocument(const Document& doc) override {
      if (*counter >= cap) {
        // Past the cap: consume and discard the rest of the page. Returning
        // false would abort the HTTP transfer mid-stream, which the transport
        // reports as a failed request and would sink the whole pass; the page
        // loop stops paginating once the cap is reached. The cursor must not
        // advance either -- these documents were not processed.
        return true;
      }
      Document copy = doc;
      engine->applyQueuedOverrides(copy);

      if (unreadOnly && (copy.flags & FLAG_SEEN) != 0) {
        // A read document in an unread-only location is never staged, but a copy
        // may already be cached from when it was unread -- reading it on another
        // device is only ever observed here, in the pull. Record a tombstone so
        // the merge drops that cached copy; without one the stale unread record
        // is never superseded and survives carry-over forever.
        //
        // Its updated_at still advances the checkpoint -- otherwise the same
        // read items would be re-walked on every sync.
        StagedRef ref{};
        copyBounded(ref.id, ID_CAP, copy.id, strlen(copy.id));
        ref.offset = 0;
        ref.length = 0;
        staged->push_back(ref);
        advanceCursor(copy);
        return true;
      }

      const size_t len = encodeDocument(copy, buffer, MAX_ENCODED_RECORD);
      if (len == 0 || !store->writeChunk(buffer, len)) {
        failed = true;
        return false;
      }
      StagedRef ref{};
      copyBounded(ref.id, ID_CAP, copy.id, strlen(copy.id));
      ref.offset = offset;
      ref.length = static_cast<uint32_t>(len);
      staged->push_back(ref);
      offset += static_cast<uint32_t>(len);
      ++(*counter);

      advanceCursor(copy);
      return true;
    }
  };

  if (!store_.beginWrite(stagingPath())) {
    return ApiStatus::NetworkError;
  }

  Sink sink;
  sink.engine = this;
  sink.store = &store_;
  sink.staged = &staged;
  sink.buffer = recordBuffer_;
  sink.highest = highestUpdatedAt;

  uint16_t sharedCount = 0;
  uint16_t feedCount = 0;

  for (const LocationPolicy& policy : SYNCED_LOCATIONS) {
    sink.unreadOnly = policy.unreadOnly;
    // Feed is additive to the shared document cap so an active firehose cannot
    // starve the Later/Shortlist views.
    sink.counter = policy.location == Location::Feed ? &feedCount : &sharedCount;
    sink.cap = policy.location == Location::Feed ? feedCap_ : documentCap_;
    if (*sink.counter >= sink.cap) {
      // An earlier location exhausted this cap; every request here would only
      // stream documents the sink refuses. Each request is a full TLS
      // handshake, so skip the location outright.
      continue;
    }

    ListQuery query;
    query.updatedAfter = checkpoint.updatedAfter;
    query.location = policy.location;
    query.limit = 100;

    std::string cursor;
    for (int page = 0; page < policy.maxPages; ++page) {
      query.pageCursor = cursor.c_str();
      ListResponse response = api_.fetchPage(query, sink);

      if (response.status == ApiStatus::RateLimited) {
        for (int retry = 0; retry < MAX_RATE_LIMIT_RETRIES && response.status == ApiStatus::RateLimited; ++retry) {
          response = api_.fetchPage(query, sink);
        }
      }
      if (response.status != ApiStatus::Ok) {
        store_.abortWrite();
        return response.status;
      }
      if (sink.failed) {
        store_.abortWrite();
        return ApiStatus::ParseError;
      }
      // Once the location's cap is reached, further pages would only stream
      // documents the sink refuses -- stop paginating.
      if (*sink.counter >= sink.cap) {
        break;
      }
      // Only a null cursor means the location is exhausted. `count` saturates at
      // 10,000 and can never be used to detect completion.
      if (response.nextPageCursor[0] == '\0') {
        break;
      }
      cursor = response.nextPageCursor;
    }
  }

  if (!store_.commitWrite()) {
    return ApiStatus::ParseError;
  }
  return ApiStatus::Ok;
}

bool ReadwiseSyncEngine::mergeIntoDocs(const std::string& sourcePath, const std::vector<StagedRef>& staged,
                                       bool carryOverExisting, bool dropExpired, std::vector<IndexEntry>& indexEntries,
                                       uint16_t& retained) {
  // Incoming documents are written first -- they are the most recently changed --
  // followed by previously cached documents that were not superseded, until the
  // cap is reached. That enforces the cap and keeps the newest data.
  if (!store_.beginWrite(docsPath())) {
    return false;
  }

  // FLAG_HAS_BODY is local-only state: the server has no idea whether we cached
  // an article's text, so an incoming record always reports it clear. Collect it
  // from the existing cache first, or every sync would silently orphan the body
  // files it had already fetched.
  struct LocalState {
    char id[ID_CAP];
    uint8_t flags;
  };
  std::vector<LocalState> localState;
  if (carryOverExisting) {
    DocsHeader previous;
    uint8_t previousHeader[DOCS_HEADER_SIZE];
    if (store_.readRange(docsPath(), 0, previousHeader, DOCS_HEADER_SIZE) == static_cast<int>(DOCS_HEADER_SIZE) &&
        decodeDocsHeader(previousHeader, DOCS_HEADER_SIZE, previous)) {
      localState.reserve(previous.recordCount);
      uint32_t scanOffset = DOCS_HEADER_SIZE;
      for (uint16_t i = 0; i < previous.recordCount; ++i) {
        const int read = store_.readRange(docsPath(), scanOffset, recordBuffer_, MAX_ENCODED_RECORD);
        if (read <= 0) {
          break;
        }
        size_t consumed = 0;
        if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed)) {
          break;
        }
        LocalState state{};
        copyBounded(state.id, ID_CAP, scratchDoc_.id, strlen(scratchDoc_.id));
        state.flags = static_cast<uint8_t>(scratchDoc_.flags & FLAG_HAS_BODY);
        localState.push_back(state);
        scanOffset += static_cast<uint32_t>(consumed);
      }
    }
  }
  auto restoreLocalFlags = [&](Document& doc) {
    for (const LocalState& state : localState) {
      if (sameId(state.id, doc.id)) {
        doc.flags |= state.flags;
        return;
      }
    }
  };

  std::vector<uint32_t> offsets;
  offsets.reserve(static_cast<size_t>(documentCap_) + feedCap_);
  uint32_t cursor = static_cast<uint32_t>(DOCS_HEADER_SIZE);
  uint16_t written = 0;
  // Feed has its own additive cap so a busy firehose cannot displace the
  // Later/Shortlist documents, and vice versa.
  uint16_t nonFeedWritten = 0;
  uint16_t feedWritten = 0;

  // The header is rewritten at the end with the real LUT offset, so a
  // placeholder goes down first to reserve the space.
  uint8_t header[DOCS_HEADER_SIZE] = {};
  encodeDocsHeader(DocsHeader(), header, sizeof(header));
  if (!store_.writeChunk(header, sizeof(header))) {
    store_.abortWrite();
    return false;
  }

  // A body is dropped as soon as its document leaves the synced locations.
  // Archived and inbox (`new`) documents are not readable from the device
  // library, so keeping their text would waste SD space indefinitely.
  auto evictBodyIfUnreadable = [&](Document& doc) {
    if (isSyncedLocation(doc.location)) {
      return;
    }
    if ((doc.flags & FLAG_HAS_BODY) == 0) {
      return;
    }
    store_.remove(bodyPath(doc.id));
    doc.flags &= static_cast<uint8_t>(~FLAG_HAS_BODY);
  };

  // Records the sync policy expires outright: `new`-location leftovers from
  // before the Inbox view was dropped, and read documents in unread-only
  // locations (feed read-removal -- overrides are applied before this check,
  // so a locally queued `seen` counts too). Gated on dropExpired so that
  // rebuildLocal, which runs on every library entry, never removes a feed
  // article the user just read; removal happens at the next sync.
  auto dropIfExpired = [&](Document& doc) {
    if (!dropExpired) {
      return false;
    }
    bool drop = doc.location == Location::New;
    if (!drop) {
      const LocationPolicy* policy = policyFor(doc.location);
      drop = policy != nullptr && policy->unreadOnly && (doc.flags & FLAG_SEEN) != 0;
    }
    if (drop && (doc.flags & FLAG_HAS_BODY) != 0) {
      store_.remove(bodyPath(doc.id));
    }
    return drop;
  };

  auto emit = [&](const Document& doc, size_t encodedLen) {
    offsets.push_back(cursor);
    cursor += static_cast<uint32_t>(encodedLen);
    IndexEntry entry{};
    entry.recordIndex = written;
    entry.location = doc.location;
    copyBounded(entry.lastMovedAt, TIMESTAMP_CAP, doc.lastMovedAt, strlen(doc.lastMovedAt));
    indexEntries.push_back(entry);
    ++written;
  };

  for (const StagedRef& ref : staged) {
    if (ref.isTombstone()) {
      // Nothing to write: the ref exists so the carry-over pass treats the
      // cached copy as superseded. Its body goes with it. Handled before the cap
      // break below, so a full cap never leaks a body.
      store_.remove(bodyPath(ref.id));
      continue;
    }
    if (nonFeedWritten >= documentCap_ && feedWritten >= feedCap_) {
      break;
    }
    const int read = store_.readRange(sourcePath, ref.offset, recordBuffer_, ref.length);
    if (read != static_cast<int>(ref.length)) {
      store_.abortWrite();
      return false;
    }
    if (!decodeDocument(recordBuffer_, ref.length, scratchDoc_)) {
      store_.abortWrite();
      return false;
    }
    restoreLocalFlags(scratchDoc_);
    const bool isFeed = scratchDoc_.location == Location::Feed;
    uint16_t& classWritten = isFeed ? feedWritten : nonFeedWritten;
    if (classWritten >= (isFeed ? feedCap_ : documentCap_)) {
      if (isFeed && (scratchDoc_.flags & FLAG_HAS_BODY) != 0) {
        // A feed item displaced by the cap is gone for good; reclaim its body.
        store_.remove(bodyPath(scratchDoc_.id));
      }
      continue;
    }
    if (dropIfExpired(scratchDoc_)) {
      continue;
    }
    evictBodyIfUnreadable(scratchDoc_);
    // Re-encode rather than replaying the source bytes: restoring local flags
    // and evicting a body both change them.
    const size_t len = encodeDocument(scratchDoc_, recordBuffer_, MAX_ENCODED_RECORD);
    if (len == 0 || !store_.writeChunk(recordBuffer_, len)) {
      store_.abortWrite();
      return false;
    }
    emit(scratchDoc_, len);
    ++classWritten;
  }

  // Carry over previously cached documents the pull did not supersede.
  DocsHeader existing;
  uint8_t existingHeader[DOCS_HEADER_SIZE];
  if (carryOverExisting &&
      store_.readRange(docsPath(), 0, existingHeader, DOCS_HEADER_SIZE) == static_cast<int>(DOCS_HEADER_SIZE) &&
      decodeDocsHeader(existingHeader, DOCS_HEADER_SIZE, existing)) {
    uint32_t readOffset = DOCS_HEADER_SIZE;
    for (uint16_t i = 0; i < existing.recordCount && (nonFeedWritten < documentCap_ || feedWritten < feedCap_); ++i) {
      const int read = store_.readRange(docsPath(), readOffset, recordBuffer_, MAX_ENCODED_RECORD);
      if (read <= 0) {
        break;
      }
      size_t consumed = 0;
      if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed)) {
        break;
      }
      readOffset += static_cast<uint32_t>(consumed);

      bool superseded = false;
      for (const StagedRef& ref : staged) {
        if (sameId(ref.id, scratchDoc_.id)) {
          superseded = true;
          break;
        }
      }
      if (superseded) {
        continue;
      }
      applyQueuedOverrides(scratchDoc_);
      const bool isFeed = scratchDoc_.location == Location::Feed;
      uint16_t& classWritten = isFeed ? feedWritten : nonFeedWritten;
      if (classWritten >= (isFeed ? feedCap_ : documentCap_)) {
        if (isFeed && (scratchDoc_.flags & FLAG_HAS_BODY) != 0) {
          // Displaced by newer staged feed items; reclaim the body.
          store_.remove(bodyPath(scratchDoc_.id));
        }
        continue;
      }
      if (dropIfExpired(scratchDoc_)) {
        continue;
      }
      evictBodyIfUnreadable(scratchDoc_);
      const size_t len = encodeDocument(scratchDoc_, recordBuffer_, MAX_ENCODED_RECORD);
      if (len == 0 || !store_.writeChunk(recordBuffer_, len)) {
        store_.abortWrite();
        return false;
      }
      emit(scratchDoc_, len);
      ++classWritten;
    }
  }

  // The LUT goes last so the record area is written in one forward pass, the
  // same shape as book.bin.
  const uint32_t lutOffset = cursor;
  for (uint32_t offset : offsets) {
    uint8_t entry[4] = {static_cast<uint8_t>(offset & 0xFF), static_cast<uint8_t>((offset >> 8) & 0xFF),
                        static_cast<uint8_t>((offset >> 16) & 0xFF), static_cast<uint8_t>((offset >> 24) & 0xFF)};
    if (!store_.writeChunk(entry, sizeof(entry))) {
      store_.abortWrite();
      return false;
    }
  }

  // Patch the placeholder header now the LUT offset and record count are known,
  // then commit. Patching before the rename keeps the commit a single atomic
  // step: a crash here leaves the previous docs.bin untouched.
  DocsHeader finalHeader;
  finalHeader.lutOffset = lutOffset;
  finalHeader.recordCount = written;
  encodeDocsHeader(finalHeader, header, sizeof(header));
  if (!store_.patchWrite(0, header, sizeof(header))) {
    store_.abortWrite();
    return false;
  }

  if (!store_.commitWrite()) {
    return false;
  }

  retained = written;
  return true;
}

bool ReadwiseSyncEngine::writeIndexes(std::vector<IndexEntry>& indexEntries) {
  // The Inbox view was dropped; clear the orphaned index a pre-upgrade sync may
  // have left behind. Failure is ignored -- the file usually does not exist.
  store_.remove(indexPath(Location::New));

  for (const LocationPolicy& policy : SYNCED_LOCATIONS) {
    const Location location = policy.location;
    std::vector<const IndexEntry*> matching;
    matching.reserve(indexEntries.size());
    for (const IndexEntry& entry : indexEntries) {
      if (entry.location == location) {
        matching.push_back(&entry);
      }
    }
    // Newest first. ISO 8601 compares correctly as a string, so no date parsing
    // is needed here.
    std::sort(matching.begin(), matching.end(), [](const IndexEntry* a, const IndexEntry* b) {
      return strncmp(a->lastMovedAt, b->lastMovedAt, TIMESTAMP_CAP) > 0;
    });

    std::vector<uint8_t> buffer;
    buffer.resize(INDEX_HEADER_SIZE + matching.size() * INDEX_ENTRY_SIZE);
    encodeIndexHeader(static_cast<uint16_t>(matching.size()), buffer.data(), buffer.size());
    for (size_t i = 0; i < matching.size(); ++i) {
      encodeIndexEntry(matching[i]->recordIndex, buffer.data() + INDEX_HEADER_SIZE + i * INDEX_ENTRY_SIZE);
    }
    if (!store_.writeAll(indexPath(location), buffer.data(), buffer.size())) {
      return false;
    }
  }
  return true;
}

bool ReadwiseSyncEngine::commitCheckpoint(const char* updatedAfter, uint16_t docCount) {
  Checkpoint checkpoint;
  copyBounded(checkpoint.updatedAfter, TIMESTAMP_CAP, updatedAfter, strlen(updatedAfter));
  checkpoint.docCount = docCount;

  uint8_t buffer[CHECKPOINT_SIZE];
  if (encodeCheckpoint(checkpoint, buffer, sizeof(buffer)) != CHECKPOINT_SIZE) {
    return false;
  }
  return store_.writeAll(checkpointPath(), buffer, CHECKPOINT_SIZE);
}

bool ReadwiseSyncEngine::readIndexPage(Location location, uint16_t offset, uint16_t count, std::vector<Document>& out) {
  out.clear();

  uint8_t header[INDEX_HEADER_SIZE];
  if (store_.readRange(indexPath(location), 0, header, INDEX_HEADER_SIZE) != static_cast<int>(INDEX_HEADER_SIZE)) {
    return false;
  }
  uint16_t total = 0;
  if (!decodeIndexHeader(header, INDEX_HEADER_SIZE, total) || offset >= total) {
    return false;
  }

  DocsHeader docsHeader;
  uint8_t docsHeaderBuffer[DOCS_HEADER_SIZE];
  if (store_.readRange(docsPath(), 0, docsHeaderBuffer, DOCS_HEADER_SIZE) != static_cast<int>(DOCS_HEADER_SIZE) ||
      !decodeDocsHeader(docsHeaderBuffer, DOCS_HEADER_SIZE, docsHeader)) {
    return false;
  }

  const uint16_t available = static_cast<uint16_t>(total - offset);
  const uint16_t wanted = count < available ? count : available;
  out.reserve(wanted);

  // Only the requested slice of the index is read, then one seek per document.
  // Nothing outside the visible page is touched.
  for (uint16_t i = 0; i < wanted; ++i) {
    uint8_t entry[INDEX_ENTRY_SIZE];
    const size_t entryOffset = INDEX_HEADER_SIZE + static_cast<size_t>(offset + i) * INDEX_ENTRY_SIZE;
    if (store_.readRange(indexPath(location), entryOffset, entry, INDEX_ENTRY_SIZE) !=
        static_cast<int>(INDEX_ENTRY_SIZE)) {
      return false;
    }
    const uint16_t recordIndex = decodeIndexEntry(entry);
    if (recordIndex >= docsHeader.recordCount) {
      return false;
    }

    uint8_t lutEntry[4];
    const size_t lutOffset = docsHeader.lutOffset + static_cast<size_t>(recordIndex) * 4;
    if (store_.readRange(docsPath(), lutOffset, lutEntry, 4) != 4) {
      return false;
    }
    const uint32_t recordOffset = static_cast<uint32_t>(lutEntry[0]) | (static_cast<uint32_t>(lutEntry[1]) << 8) |
                                  (static_cast<uint32_t>(lutEntry[2]) << 16) |
                                  (static_cast<uint32_t>(lutEntry[3]) << 24);

    const int read = store_.readRange(docsPath(), recordOffset, recordBuffer_, MAX_ENCODED_RECORD);
    if (read <= 0 || !decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_)) {
      return false;
    }
    out.push_back(scratchDoc_);
  }
  return true;
}

bool ReadwiseSyncEngine::rebuildLocal() {
  if (!journal_.load()) {
    return false;
  }
  // The library calls this on every entry; with nothing queued there is
  // nothing to apply, and a full docs.bin + index rewrite would be pure SD
  // wear on ordinary navigation.
  if (journal_.empty()) {
    return true;
  }
  // No staged documents: everything carries over from the existing docs.bin,
  // and the carry-over path applies the queued overrides and body eviction.
  std::vector<IndexEntry> indexEntries;
  indexEntries.reserve(static_cast<size_t>(documentCap_) + feedCap_);
  uint16_t retained = 0;
  // dropExpired is false: a feed article the user just read must stay openable
  // until the next sync, and rebuildLocal runs on every library entry.
  if (!mergeIntoDocs(docsPath(), {}, /*carryOverExisting=*/true, /*dropExpired=*/false, indexEntries, retained)) {
    return false;
  }
  return writeIndexes(indexEntries);
}

bool ReadwiseSyncEngine::findDocument(const char* id, Document& out) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  DocsHeader header;
  uint8_t headerBuffer[DOCS_HEADER_SIZE];
  if (store_.readRange(docsPath(), 0, headerBuffer, DOCS_HEADER_SIZE) != static_cast<int>(DOCS_HEADER_SIZE) ||
      !decodeDocsHeader(headerBuffer, DOCS_HEADER_SIZE, header)) {
    return false;
  }
  uint32_t offset = DOCS_HEADER_SIZE;
  for (uint16_t i = 0; i < header.recordCount; ++i) {
    const int read = store_.readRange(docsPath(), offset, recordBuffer_, MAX_ENCODED_RECORD);
    if (read <= 0) {
      return false;
    }
    size_t consumed = 0;
    if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed)) {
      return false;
    }
    if (sameId(scratchDoc_.id, id)) {
      out = scratchDoc_;
      return true;
    }
    offset += static_cast<uint32_t>(consumed);
  }
  return false;
}

ReadwiseSyncEngine::BodySyncOutcome ReadwiseSyncEngine::downloadMissingBodies(const BodySyncHooks& hooks) {
  BodySyncOutcome outcome;

  // Collect the ids first: setBodyCached patches docs.bin while we work, and
  // interleaving fetches with an open record scan would be fragile. At the cap
  // this is a few KB of ids.
  struct MissingId {
    char id[ID_CAP];
  };
  std::vector<MissingId> missing;
  {
    DocsHeader header;
    uint8_t headerBuffer[DOCS_HEADER_SIZE];
    if (store_.readRange(docsPath(), 0, headerBuffer, DOCS_HEADER_SIZE) != static_cast<int>(DOCS_HEADER_SIZE) ||
        !decodeDocsHeader(headerBuffer, DOCS_HEADER_SIZE, header)) {
      // No cache yet: nothing to download is a success, not a failure.
      outcome.ok = true;
      return outcome;
    }
    missing.reserve(header.recordCount);
    uint32_t offset = DOCS_HEADER_SIZE;
    for (uint16_t i = 0; i < header.recordCount; ++i) {
      const int read = store_.readRange(docsPath(), offset, recordBuffer_, MAX_ENCODED_RECORD);
      if (read <= 0) {
        break;
      }
      size_t consumed = 0;
      if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed)) {
        break;
      }
      // Only documents readable from the library get a body: fetching for an
      // archived leftover would be wasted transfer, and the merge would evict
      // it again on the next sync anyway. Feed passes -- its items read
      // offline like everything else.
      if (!isSyncedLocation(scratchDoc_.location)) {
        offset += static_cast<uint32_t>(consumed);
        continue;
      }
      // Missing means no flag, or a flag whose file has gone (cache cleared).
      if ((scratchDoc_.flags & FLAG_HAS_BODY) == 0 || !store_.exists(bodyPath(scratchDoc_.id))) {
        MissingId entry{};
        copyBounded(entry.id, ID_CAP, scratchDoc_.id, strlen(scratchDoc_.id));
        missing.push_back(entry);
      }
      offset += static_cast<uint32_t>(consumed);
    }
  }

  outcome.total = static_cast<uint16_t>(missing.size());
  if (missing.empty()) {
    outcome.ok = true;
    return outcome;
  }
  store_.ensureDir(baseDir_ + "/bodies");

  // Publish the denominator before the first fetch: a document can take many
  // seconds (or a rate-limit wait), and the caller's popup would otherwise sit
  // at 0/0 until one completes.
  if (hooks.onProgress != nullptr) {
    hooks.onProgress(hooks.ctx, 0, outcome.total);
  }

  uint16_t done = 0;
  for (const MissingId& entry : missing) {
    ApiStatus status = ApiStatus::NetworkError;
    for (int attempt = 0; attempt < 1 + MAX_RATE_LIMIT_RETRIES; ++attempt) {
      auto writer = makeUniqueNoThrow<BodyTextWriter>(store_, bodyPath(entry.id));
      if (!writer) {
        status = ApiStatus::LowMemory;
        break;
      }
      uint16_t retryAfter = 0;
      status = api_.fetchBody(entry.id, *writer, &retryAfter);
      if (status == ApiStatus::Ok && !writer->committed()) {
        // 200 with no html_content string: nothing to retry.
        status = ApiStatus::ParseError;
      }
      if (status != ApiStatus::RateLimited) {
        break;
      }
      // Throttled: wait out the server's ask (bounded) and retry this
      // document once. The activity's delay keeps the UI task serviced.
      if (hooks.sleepMs != nullptr) {
        uint32_t waitMs = retryAfter != 0 ? static_cast<uint32_t>(retryAfter) * 1000u : DEFAULT_RATE_LIMIT_WAIT_MS;
        if (waitMs > RATE_LIMIT_WAIT_CAP_MS) {
          waitMs = RATE_LIMIT_WAIT_CAP_MS;
        }
        hooks.sleepMs(hooks.ctx, waitMs);
      }
    }

    if (status == ApiStatus::Ok) {
      setBodyCached(entry.id, true);
      ++outcome.downloaded;
    } else if (status == ApiStatus::AuthFailed || status == ApiStatus::NoCredentials) {
      // Every remaining fetch would fail identically; abandon the pass. What
      // already downloaded stays cached.
      outcome.status = status;
      return outcome;
    } else {
      // Skip this document; the library's on-demand path is the retry.
      ++outcome.failed;
    }
    ++done;
    if (hooks.onProgress != nullptr) {
      hooks.onProgress(hooks.ctx, done, outcome.total);
    }
  }

  outcome.ok = true;
  return outcome;
}

bool ReadwiseSyncEngine::setBodyCached(const char* id, bool cached) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  DocsHeader header;
  uint8_t headerBuffer[DOCS_HEADER_SIZE];
  if (store_.readRange(docsPath(), 0, headerBuffer, DOCS_HEADER_SIZE) != static_cast<int>(DOCS_HEADER_SIZE) ||
      !decodeDocsHeader(headerBuffer, DOCS_HEADER_SIZE, header)) {
    return false;
  }
  uint32_t offset = DOCS_HEADER_SIZE;
  for (uint16_t i = 0; i < header.recordCount; ++i) {
    const int read = store_.readRange(docsPath(), offset, recordBuffer_, MAX_ENCODED_RECORD);
    if (read <= 0) {
      return false;
    }
    size_t consumed = 0;
    if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed)) {
      return false;
    }
    if (sameId(scratchDoc_.id, id)) {
      uint8_t flags = scratchDoc_.flags;
      if (cached) {
        flags |= FLAG_HAS_BODY;
      } else {
        flags &= static_cast<uint8_t>(~FLAG_HAS_BODY);
      }
      if (flags == scratchDoc_.flags) {
        return true;
      }
      // The flags byte is the final byte of the encoded record.
      const size_t flagsOffset = offset + consumed - 1;
      return store_.writeRange(docsPath(), flagsOffset, &flags, 1);
    }
    offset += static_cast<uint32_t>(consumed);
  }
  return false;
}

uint16_t ReadwiseSyncEngine::indexCount(Location location) {
  uint8_t header[INDEX_HEADER_SIZE];
  if (store_.readRange(indexPath(location), 0, header, INDEX_HEADER_SIZE) != static_cast<int>(INDEX_HEADER_SIZE)) {
    return 0;
  }
  uint16_t count = 0;
  if (!decodeIndexHeader(header, INDEX_HEADER_SIZE, count)) {
    return 0;
  }
  return count;
}

SyncOutcome ReadwiseSyncEngine::reconcile() {
  SyncOutcome outcome;
  outcome.failedStage = SyncStage::Pulling;

  // Collect every id the server still reports across the synced locations.
  struct IdSink : DocumentSink {
    std::vector<std::string>* ids;
    bool onDocument(const Document& doc) override {
      ids->push_back(doc.id);
      return true;
    }
  };

  std::vector<std::string> liveIds;
  liveIds.reserve(documentCap_);
  IdSink sink;
  sink.ids = &liveIds;

  // Feed is not swept: it exceeds the `count` cap on its own, and its items
  // expire via the unread filter and the feed cap instead of by deletion.
  for (const LocationPolicy& policy : SYNCED_LOCATIONS) {
    if (!policy.reconcileSweep) {
      continue;
    }
    ListQuery query;
    query.location = policy.location;
    query.limit = 100;
    std::string cursor;
    for (int page = 0; page < policy.maxPages; ++page) {
      query.pageCursor = cursor.c_str();
      const ListResponse response = api_.fetchPage(query, sink);
      if (response.status != ApiStatus::Ok) {
        // A document missing because the network failed is indistinguishable
        // from one that was deleted, so an incomplete sweep must never expire
        // anything.
        outcome.status = response.status;
        return outcome;
      }
      if (response.nextPageCursor[0] == '\0') {
        break;
      }
      cursor = response.nextPageCursor;
    }
  }

  // Only now, with a complete picture, rewrite docs.bin without the documents
  // the server no longer has.
  outcome.failedStage = SyncStage::RebuildingIndexes;
  DocsHeader existing;
  uint8_t existingHeader[DOCS_HEADER_SIZE];
  if (store_.readRange(docsPath(), 0, existingHeader, DOCS_HEADER_SIZE) != static_cast<int>(DOCS_HEADER_SIZE) ||
      !decodeDocsHeader(existingHeader, DOCS_HEADER_SIZE, existing)) {
    // Nothing cached yet: there is nothing to expire.
    outcome.ok = true;
    outcome.failedStage = SyncStage::Idle;
    return outcome;
  }

  std::vector<StagedRef> survivors;
  survivors.reserve(existing.recordCount);
  uint32_t readOffset = DOCS_HEADER_SIZE;
  for (uint16_t i = 0; i < existing.recordCount; ++i) {
    const int read = store_.readRange(docsPath(), readOffset, recordBuffer_, MAX_ENCODED_RECORD);
    if (read <= 0) {
      break;
    }
    size_t consumed = 0;
    if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed)) {
      break;
    }
    // A document in a synced-but-unswept location (feed) cannot appear in the
    // sweep, so its absence proves nothing; it is alive by definition and
    // expires only through the merge's unread/cap rules.
    const LocationPolicy* policy = policyFor(scratchDoc_.location);
    bool alive = policy != nullptr && !policy->reconcileSweep;
    for (const std::string& id : liveIds) {
      if (alive) {
        break;
      }
      if (id == scratchDoc_.id) {
        alive = true;
      }
    }
    if (alive) {
      StagedRef ref{};
      copyBounded(ref.id, ID_CAP, scratchDoc_.id, strlen(scratchDoc_.id));
      ref.offset = readOffset;
      ref.length = static_cast<uint32_t>(consumed);
      survivors.push_back(ref);
    } else {
      // The body cache goes with the document.
      store_.remove(bodyPath(scratchDoc_.id));
      ++outcome.pulled;
    }
    readOffset += static_cast<uint32_t>(consumed);
  }

  std::vector<IndexEntry> indexEntries;
  indexEntries.reserve(survivors.size());
  uint16_t retained = 0;
  if (!mergeIntoDocs(docsPath(), survivors, /*carryOverExisting=*/false, /*dropExpired=*/true, indexEntries,
                     retained) ||
      !writeIndexes(indexEntries)) {
    return outcome;
  }
  outcome.retained = retained;
  outcome.ok = true;
  outcome.failedStage = SyncStage::Idle;
  return outcome;
}

}  // namespace readwise
