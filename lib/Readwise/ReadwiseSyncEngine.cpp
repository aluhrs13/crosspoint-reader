#include "ReadwiseSyncEngine.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace readwise {
namespace {

// Guards against a runaway server: with a 100-document page and a 100-document
// cap, a sync should never need more than a handful of pages per location.
constexpr int MAX_PAGES_PER_LOCATION = 64;

// The API is rate-limited to 20 list requests per minute and answers 429 with a
// retry-after of ~16 seconds. One retry is honoured; after that the pass is
// abandoned rather than looped, because a blocking wait inside an activity can
// exceed the watchdog window and reset the device.
constexpr int MAX_RATE_LIMIT_RETRIES = 1;

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
  staged.reserve(documentCap_);
  char highestUpdatedAt[TIMESTAMP_CAP] = {};
  copyBounded(highestUpdatedAt, TIMESTAMP_CAP, checkpoint.updatedAfter, strlen(checkpoint.updatedAfter));

  const ApiStatus pullStatus = pullToStaging(checkpoint, staged, highestUpdatedAt);
  if (pullStatus != ApiStatus::Ok) {
    store_.remove(stagingPath());
    journal_.removeAcknowledged(acknowledged);
    outcome.status = pullStatus;
    return outcome;
  }
  outcome.pulled = static_cast<uint16_t>(staged.size());

  // --- 4. Merge and rebuild indexes --------------------------------------
  outcome.failedStage = SyncStage::RebuildingIndexes;
  std::vector<IndexEntry> indexEntries;
  indexEntries.reserve(documentCap_);
  uint16_t retained = 0;
  if (!mergeIntoDocs(stagingPath(), staged, /*carryOverExisting=*/true, indexEntries, retained)) {
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
    uint16_t cap = 0;
    char* highest = nullptr;
    bool failed = false;

    bool onDocument(const Document& doc) override {
      if (staged->size() >= cap) {
        // Stop early rather than buffering documents that will be dropped by the
        // cap during the merge anyway.
        return false;
      }
      Document copy = doc;
      engine->applyQueuedOverrides(copy);

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

      // The cursor is the highest updated_at actually observed, compared
      // lexicographically -- ISO 8601 orders correctly as a string, and the
      // device has no trustworthy clock to synthesize one from.
      if (strncmp(copy.updatedAt, highest, TIMESTAMP_CAP) > 0) {
        copyBounded(highest, TIMESTAMP_CAP, copy.updatedAt, strlen(copy.updatedAt));
      }
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
  sink.cap = documentCap_;
  sink.highest = highestUpdatedAt;

  for (Location location : DEFAULT_SYNCED_LOCATIONS) {
    ListQuery query;
    query.updatedAfter = checkpoint.updatedAfter;
    query.location = location;
    query.limit = 100;

    std::string cursor;
    for (int page = 0; page < MAX_PAGES_PER_LOCATION; ++page) {
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
                                       bool carryOverExisting, std::vector<IndexEntry>& indexEntries,
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
  offsets.reserve(documentCap_);
  uint32_t cursor = static_cast<uint32_t>(DOCS_HEADER_SIZE);
  uint16_t written = 0;

  // The header is rewritten at the end with the real LUT offset, so a
  // placeholder goes down first to reserve the space.
  uint8_t header[DOCS_HEADER_SIZE] = {};
  encodeDocsHeader(DocsHeader(), header, sizeof(header));
  if (!store_.writeChunk(header, sizeof(header))) {
    store_.abortWrite();
    return false;
  }

  // A body is dropped as soon as its document leaves the synced locations.
  // Archived and feed documents are not readable from the device library, so
  // keeping their text would waste SD space indefinitely.
  auto evictBodyIfUnreadable = [&](Document& doc) {
    if (doc.location != Location::Archive && doc.location != Location::Feed) {
      return;
    }
    if ((doc.flags & FLAG_HAS_BODY) == 0) {
      return;
    }
    store_.remove(bodyPath(doc.id));
    doc.flags &= static_cast<uint8_t>(~FLAG_HAS_BODY);
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
    if (written >= documentCap_) {
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
    evictBodyIfUnreadable(scratchDoc_);
    // Re-encode rather than replaying the source bytes: restoring local flags
    // and evicting a body both change them.
    const size_t len = encodeDocument(scratchDoc_, recordBuffer_, MAX_ENCODED_RECORD);
    if (len == 0 || !store_.writeChunk(recordBuffer_, len)) {
      store_.abortWrite();
      return false;
    }
    emit(scratchDoc_, len);
  }

  // Carry over previously cached documents the pull did not supersede.
  DocsHeader existing;
  uint8_t existingHeader[DOCS_HEADER_SIZE];
  if (carryOverExisting &&
      store_.readRange(docsPath(), 0, existingHeader, DOCS_HEADER_SIZE) == static_cast<int>(DOCS_HEADER_SIZE) &&
      decodeDocsHeader(existingHeader, DOCS_HEADER_SIZE, existing)) {
    uint32_t readOffset = DOCS_HEADER_SIZE;
    for (uint16_t i = 0; i < existing.recordCount && written < documentCap_; ++i) {
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
      evictBodyIfUnreadable(scratchDoc_);
      const size_t len = encodeDocument(scratchDoc_, recordBuffer_, MAX_ENCODED_RECORD);
      if (len == 0 || !store_.writeChunk(recordBuffer_, len)) {
        store_.abortWrite();
        return false;
      }
      emit(scratchDoc_, len);
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
  for (Location location : DEFAULT_SYNCED_LOCATIONS) {
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
  // No staged documents: everything carries over from the existing docs.bin,
  // and the carry-over path applies the queued overrides and body eviction.
  std::vector<IndexEntry> indexEntries;
  indexEntries.reserve(documentCap_);
  uint16_t retained = 0;
  if (!mergeIntoDocs(docsPath(), {}, /*carryOverExisting=*/true, indexEntries, retained)) {
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

  for (Location location : DEFAULT_SYNCED_LOCATIONS) {
    ListQuery query;
    query.location = location;
    query.limit = 100;
    std::string cursor;
    for (int page = 0; page < MAX_PAGES_PER_LOCATION; ++page) {
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
    bool alive = false;
    for (const std::string& id : liveIds) {
      if (id == scratchDoc_.id) {
        alive = true;
        break;
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
  if (!mergeIntoDocs(docsPath(), survivors, /*carryOverExisting=*/false, indexEntries, retained) ||
      !writeIndexes(indexEntries)) {
    return outcome;
  }
  outcome.retained = retained;
  outcome.ok = true;
  outcome.failedStage = SyncStage::Idle;
  return outcome;
}

}  // namespace readwise
