#include "ReadwiseSyncEngine.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "ArticleAssembler.h"
#include "ArticleBodyWriter.h"

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

void formatOpStep(char* step, size_t stepCap, const PendingOp& op) {
  if (step == nullptr || stepCap == 0) {
    return;
  }
  const char* verb = "update";
  const char* where = nullptr;
  switch (op.op) {
    case OpType::Delete:
      verb = "delete";
      break;
    case OpType::SetSeen:
      verb = "seen";
      break;
    case OpType::SetLocation:
      verb = "move";
      where = locationName(static_cast<Location>(op.payload));
      break;
  }
  if (where != nullptr && where[0] != '\0') {
    snprintf(step, stepCap, "%s %s %s", verb, where, op.id);
  } else {
    snprintf(step, stepCap, "%s %s", verb, op.id);
  }
}

void writeStep(char* step, size_t stepCap, const char* text) {
  if (step == nullptr || stepCap == 0) {
    return;
  }
  snprintf(step, stepCap, "%s", text != nullptr ? text : "");
}

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
    case ApiStatus::NotFound:
      return "not found";
    case ApiStatus::Rejected:
      return "rejected";
  }
  return "unknown";
}

ReadwiseSyncEngine::ReadwiseSyncEngine(ReadwiseApi& api, ReadwiseFileStore& store, std::string baseDir)
    : api_(api), store_(store), baseDir_(std::move(baseDir)), journal_(store, baseDir_ + "/journal.bin") {}

std::string ReadwiseSyncEngine::indexPath(Location location) const {
  return baseDir_ + "/index_" + locationName(location) + ".bin";
}

std::string ReadwiseSyncEngine::articleDir(const char* id) const {
  return baseDir_ + "/bodies/" + (id != nullptr ? id : "");
}

std::string ReadwiseSyncEngine::bodyPath(const char* id) const { return articleDir(id) + "/article.epub"; }

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

bool ReadwiseSyncEngine::queueDelete(const char* id, const char* remoteRev) {
  if (!isValidDocumentId(id)) {
    return false;
  }
  // append writes whatever is already in memory. Load first so a caller that
  // has not queued anything this session cannot replace the journal with one
  // entry.
  if (!journal_.load()) {
    return false;
  }
  return journal_.append(OpType::Delete, id, 0, remoteRev);
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

void ReadwiseSyncEngine::noteFailure(SyncOutcome& outcome, ApiStatus status, const char* step) {
  outcome.status = status;
  const char* http = api_.lastDetail();
  const bool haveHttp = http != nullptr && http[0] != '\0';
  const bool haveStep = step != nullptr && step[0] != '\0';
  if (haveStep && haveHttp) {
    snprintf(outcome.detail, sizeof(outcome.detail), "%s: %s", step, http);
  } else if (haveStep) {
    snprintf(outcome.detail, sizeof(outcome.detail), "%s", step);
  } else if (haveHttp) {
    snprintf(outcome.detail, sizeof(outcome.detail), "%s", http);
  } else {
    outcome.detail[0] = '\0';
  }
}

SyncOutcome ReadwiseSyncEngine::sync() {
  SyncOutcome outcome;

  if (!store_.ensureDir(baseDir_)) {
    outcome.failedStage = SyncStage::Idle;
    noteFailure(outcome, ApiStatus::Ok, "storage");
    return outcome;
  }

  // --- 1. Coalesce --------------------------------------------------------
  outcome.failedStage = SyncStage::Coalescing;
  if (!journal_.load()) {
    noteFailure(outcome, ApiStatus::Ok, "journal");
    return outcome;
  }
  journal_.coalesce();

  // --- 2. Push, oldest-first ---------------------------------------------
  // Clips go first so a quote is posted before a queued delete forgets the
  // document. The file itself lives outside the article directory.
  outcome.failedStage = SyncStage::Pushing;
  char step[64];
  step[0] = '\0';
  HighlightPushStats highlightStats;
  const ApiStatus highlightStatus = pushPendingHighlights(step, sizeof(step), highlightStats);
  outcome.highlightsSent = highlightStats.sent;
  outcome.highlightsFailed = highlightStats.failed;
  copyBounded(outcome.highlightDetail, sizeof(outcome.highlightDetail), highlightStats.detail,
              strlen(highlightStats.detail));
  LOG_INF("RWSYNC", "Highlights sent=%u failed=%u%s%s", static_cast<unsigned>(outcome.highlightsSent),
          static_cast<unsigned>(outcome.highlightsFailed), outcome.highlightDetail[0] != '\0' ? " " : "",
          outcome.highlightDetail);
  if (highlightStatus != ApiStatus::Ok) {
    noteFailure(outcome, highlightStatus, step[0] != '\0' ? step : "highlight");
    return outcome;
  }
  std::vector<uint32_t> acknowledged;
  acknowledged.reserve(journal_.entries().size());
  // 404s the caller chose to drop. Kept out of `acknowledged` until they are
  // persisted, so a later failure does not forget a drop the user already made,
  // and a failed persist is retried on the way out.
  std::vector<uint32_t> skipped;
  auto persistResolved = [&]() {
    if (!skipped.empty()) {
      acknowledged.insert(acknowledged.end(), skipped.begin(), skipped.end());
      skipped.clear();
    }
    return journal_.removeAcknowledged(acknowledged);
  };
  for (const PendingOp& op : journal_.entries()) {
    ApiStatus status = api_.pushOp(op);
    if (status == ApiStatus::RateLimited) {
      for (int retry = 0; retry < MAX_RATE_LIMIT_RETRIES && status == ApiStatus::RateLimited; ++retry) {
        status = api_.pushOp(op);
      }
    }
    if (status == ApiStatus::NotFound && notFoundHooks_.onNotFound != nullptr) {
      // Copy the title out before the hook returns: the prompt can run for as
      // long as the user needs, and forgetDocument below reuses scratchDoc_.
      char title[TITLE_CAP];
      title[0] = '\0';
      if (findDocument(op.id, scratchDoc_)) {
        copyBounded(title, TITLE_CAP, scratchDoc_.title, strlen(scratchDoc_.title));
      }
      const char* detail = api_.lastDetail();
      const bool drop = notFoundHooks_.onNotFound(notFoundHooks_.ctx, op, title, detail != nullptr ? detail : "");
      if (drop) {
        // The server no longer has this document. Drop the local copy so it
        // does not keep a slot, then drop the journal op so the next sync
        // does not ask again.
        forgetDocument(op.id);
        skipped.push_back(op.seq);
        continue;
      }
    }
    if (status != ApiStatus::Ok) {
      // Name the op before removeAcknowledged, which rewrites the vector this
      // reference points into.
      formatOpStep(step, sizeof(step), op);
      // Persist what was acknowledged before giving up, so the work already
      // accepted by the server is not repeated on the next pass. Ops the
      // caller already chose to drop go with them.
      persistResolved();
      noteFailure(outcome, status, step);
      return outcome;
    }
    // The server has accepted the delete. Drop the local copy before the
    // journal entry is acknowledged: an incremental pull will not mention the
    // deletion, so a record left behind would stay forever.
    if (op.op == OpType::Delete && !forgetDocument(op.id)) {
      persistResolved();
      snprintf(step, sizeof(step), "delete local %s", op.id);
      noteFailure(outcome, ApiStatus::ServerError, step);
      return outcome;
    }
    acknowledged.push_back(op.seq);
    ++outcome.pushed;
  }

  // Drop 404s before the merge. A successful push stays queued until commit so
  // its override still wins; a dropped 404 must not.
  if (!skipped.empty() && journal_.removeAcknowledged(skipped)) {
    skipped.clear();
  }

  // --- 3. Pull ------------------------------------------------------------
  outcome.failedStage = SyncStage::Pulling;
  Checkpoint checkpoint;
  loadCheckpoint(checkpoint);

  std::vector<StagedRef> staged;
  staged.reserve(static_cast<size_t>(documentCap_) + feedCap_);
  char highestUpdatedAt[TIMESTAMP_CAP] = {};
  copyBounded(highestUpdatedAt, TIMESTAMP_CAP, checkpoint.updatedAfter, strlen(checkpoint.updatedAfter));

  step[0] = '\0';
  // Later and Shortlist share the document cap. Once both are empty, pull those
  // locations without updatedAfter so the cap fills with what is still on the
  // shelf. The checkpoint is left in place: Feed, and the next incremental
  // sync, keep their window.
  const bool refillNonFeed = nonFeedShelfEmpty();
  const ApiStatus pullStatus = pullToStaging(checkpoint, staged, highestUpdatedAt, step, sizeof(step), refillNonFeed);
  if (pullStatus != ApiStatus::Ok) {
    store_.remove(stagingPath());
    persistResolved();
    noteFailure(outcome, pullStatus, step[0] != '\0' ? step : "pull");
    return outcome;
  }
  // Tombstones (seen items in unread-only locations) are evictions, not
  // pulled documents; only real records count.
  outcome.pulled = 0;
  for (const StagedRef& ref : staged) {
    if (ref.length > 0) {
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
    persistResolved();
    noteFailure(outcome, ApiStatus::Ok, "rebuild");
    return outcome;
  }
  if (!writeIndexes(indexEntries)) {
    store_.remove(stagingPath());
    persistResolved();
    noteFailure(outcome, ApiStatus::Ok, "indexes");
    return outcome;
  }
  outcome.retained = retained;

  // --- 5. Commit ----------------------------------------------------------
  outcome.failedStage = SyncStage::Committing;
  if (!commitCheckpoint(highestUpdatedAt, retained)) {
    persistResolved();
    noteFailure(outcome, ApiStatus::Ok, "checkpoint");
    return outcome;
  }

  store_.remove(stagingPath());
  if (!persistResolved()) {
    // The sync itself committed; failing to trim the journal only means the
    // acknowledged ops are retried next pass, which is harmless: location and
    // seen write the same value again, and a repeated delete is treated as
    // success when the server says the document is already gone.
    outcome.ok = true;
    outcome.failedStage = SyncStage::Idle;
    return outcome;
  }

  outcome.ok = true;
  outcome.failedStage = SyncStage::Idle;
  return outcome;
}

bool ReadwiseSyncEngine::nonFeedShelfEmpty() {
  DocsHeader header;
  uint8_t headerBuffer[DOCS_HEADER_SIZE];
  if (store_.readRange(docsPath(), 0, headerBuffer, DOCS_HEADER_SIZE) != static_cast<int>(DOCS_HEADER_SIZE) ||
      !decodeDocsHeader(headerBuffer, DOCS_HEADER_SIZE, header)) {
    return true;
  }
  uint32_t offset = DOCS_HEADER_SIZE;
  for (uint16_t i = 0; i < header.recordCount; ++i) {
    const int read = store_.readRange(docsPath(), offset, recordBuffer_, MAX_ENCODED_RECORD);
    if (read <= 0) {
      return false;
    }
    size_t consumed = 0;
    if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed) || consumed == 0) {
      return false;
    }
    offset += static_cast<uint32_t>(consumed);
    applyQueuedOverrides(scratchDoc_);
    if (journal_.findLatest(scratchDoc_.id, OpType::Delete) != nullptr) {
      continue;
    }
    if (scratchDoc_.location == Location::Later || scratchDoc_.location == Location::Shortlist) {
      return false;
    }
  }
  return true;
}

ApiStatus ReadwiseSyncEngine::pullToStaging(const Checkpoint& checkpoint, std::vector<StagedRef>& staged,
                                            char* highestUpdatedAt, char* step, size_t stepCap, bool refillNonFeed) {
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
        // A read document in an unread-only location is never staged, but its
        // updated_at still advances the checkpoint -- otherwise the same read
        // items would be re-walked on every sync.
        //
        // It still leaves a zero-length tombstone: without one, an item read
        // on ANOTHER device is skipped here, the merge then finds no staged
        // replacement, and the stale unseen record is carried over forever --
        // the item never disappears from the feed.
        StagedRef tombstone{};
        copyBounded(tombstone.id, ID_CAP, copy.id, strlen(copy.id));
        tombstone.length = 0;
        staged->push_back(tombstone);
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
    writeStep(step, stepCap, "pull storage");
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
    query.updatedAfter = (refillNonFeed && policy.location != Location::Feed) ? "" : checkpoint.updatedAfter;
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
      if (response.status != ApiStatus::Ok || sink.failed) {
        store_.abortWrite();
        if (step != nullptr && stepCap > 0) {
          snprintf(step, stepCap, "pull %s", locationName(policy.location));
        }
        return response.status != ApiStatus::Ok ? response.status : ApiStatus::ParseError;
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
    writeStep(step, stepCap, "pull commit");
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
    dropArticleDir(doc.id);
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
      dropArticleDir(doc.id);
    }
    return drop;
  };

  // A queued delete stays in the file until the server accepts it. The list
  // reads location indexes, so leaving it out of those drops the row now.
  auto queuedDelete = [&](const Document& doc) { return journal_.findLatest(doc.id, OpType::Delete) != nullptr; };

  auto emit = [&](const Document& doc, size_t encodedLen) {
    offsets.push_back(cursor);
    cursor += static_cast<uint32_t>(encodedLen);
    if (!queuedDelete(doc)) {
      IndexEntry entry{};
      entry.recordIndex = written;
      entry.location = doc.location;
      copyBounded(entry.lastMovedAt, TIMESTAMP_CAP, doc.lastMovedAt, strlen(doc.lastMovedAt));
      indexEntries.push_back(entry);
    }
    ++written;
  };

  for (const StagedRef& ref : staged) {
    if (ref.length == 0) {
      continue;  // tombstone: nothing staged; its work happens in carry-over
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
    if (droppingId_[0] != '\0' && sameId(scratchDoc_.id, droppingId_)) {
      dropArticleDir(scratchDoc_.id);
      continue;
    }
    restoreLocalFlags(scratchDoc_);
    const bool isFeed = scratchDoc_.location == Location::Feed;
    const bool hideQueuedDelete = queuedDelete(scratchDoc_);
    uint16_t& classWritten = isFeed ? feedWritten : nonFeedWritten;
    if (!hideQueuedDelete && classWritten >= (isFeed ? feedCap_ : documentCap_)) {
      if (isFeed && (scratchDoc_.flags & FLAG_HAS_BODY) != 0) {
        // A feed item displaced by the cap is gone for good; reclaim its body.
        dropArticleDir(scratchDoc_.id);
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
    if (!hideQueuedDelete) {
      ++classWritten;
    }
  }

  // Carry over previously cached documents the pull did not supersede.
  DocsHeader existing;
  uint8_t existingHeader[DOCS_HEADER_SIZE];
  if (carryOverExisting &&
      store_.readRange(docsPath(), 0, existingHeader, DOCS_HEADER_SIZE) == static_cast<int>(DOCS_HEADER_SIZE) &&
      decodeDocsHeader(existingHeader, DOCS_HEADER_SIZE, existing)) {
    uint32_t readOffset = DOCS_HEADER_SIZE;
    // Scan the whole file. The cap check below skips ordinary documents once
    // a shelf is full, but a queued delete past that point still has to be
    // copied or the local retry copy disappears before the server accepts it.
    for (uint16_t i = 0; i < existing.recordCount; ++i) {
      const int read = store_.readRange(docsPath(), readOffset, recordBuffer_, MAX_ENCODED_RECORD);
      if (read <= 0) {
        break;
      }
      size_t consumed = 0;
      if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed)) {
        break;
      }
      readOffset += static_cast<uint32_t>(consumed);

      if (droppingId_[0] != '\0' && sameId(scratchDoc_.id, droppingId_)) {
        dropArticleDir(scratchDoc_.id);
        continue;
      }

      const StagedRef* superseding = nullptr;
      for (const StagedRef& ref : staged) {
        if (sameId(ref.id, scratchDoc_.id)) {
          superseding = &ref;
          break;
        }
      }
      if (superseding != nullptr) {
        if (superseding->length == 0) {
          // A tombstone: the pull saw this document seen in an unread-only
          // location and deliberately staged no replacement. The record dies
          // here, and its body with it -- no surviving copy carries the flag.
          dropArticleDir(scratchDoc_.id);
        }
        continue;
      }
      applyQueuedOverrides(scratchDoc_);
      const bool isFeed = scratchDoc_.location == Location::Feed;
      const bool hideQueuedDelete = queuedDelete(scratchDoc_);
      uint16_t& classWritten = isFeed ? feedWritten : nonFeedWritten;
      if (!hideQueuedDelete && classWritten >= (isFeed ? feedCap_ : documentCap_)) {
        if (isFeed && (scratchDoc_.flags & FLAG_HAS_BODY) != 0) {
          // Displaced by newer staged feed items; reclaim the body.
          dropArticleDir(scratchDoc_.id);
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
      if (!hideQueuedDelete) {
        ++classWritten;
      }
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

bool ReadwiseSyncEngine::collectQueued(std::vector<QueuedDocument>& out) {
  out.clear();
  if (!journal_.load()) {
    return false;
  }
  out.reserve(QUEUED_LIST_MAX);
  for (const PendingOp& op : journal_.entries()) {
    if (out.size() >= QUEUED_LIST_MAX) {
      break;
    }
    bool seen = false;
    for (const QueuedDocument& row : out) {
      if (sameId(row.id, op.id)) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    const PendingOp* deleted = journal_.findLatest(op.id, OpType::Delete);
    const PendingOp* location = journal_.findLatest(op.id, OpType::SetLocation);
    const bool archived = location != nullptr && static_cast<Location>(location->payload) == Location::Archive;
    if (deleted == nullptr && !archived) {
      continue;
    }
    QueuedDocument row{};
    copyBounded(row.id, ID_CAP, op.id, strlen(op.id));
    row.deleted = deleted != nullptr;
    if (findDocument(op.id, scratchDoc_) && scratchDoc_.title[0] != '\0') {
      copyBounded(row.title, TITLE_CAP, scratchDoc_.title, strlen(scratchDoc_.title));
      copyBounded(row.author, AUTHOR_CAP, scratchDoc_.author, strlen(scratchDoc_.author));
    } else {
      copyBounded(row.title, TITLE_CAP, op.id, strlen(op.id));
    }
    out.push_back(row);
  }
  return true;
}

bool ReadwiseSyncEngine::undoQueued(const char* id) {
  if (!isValidDocumentId(id)) {
    return false;
  }
  if (!journal_.load()) {
    return false;
  }
  return journal_.dropQueuedDocument(id);
}

bool ReadwiseSyncEngine::collectLibraryCounts(LibraryCounts& out) {
  static_assert(static_cast<int>(Category::Article) == 0, "named slots are indexed by Category");
  static_assert(static_cast<int>(Category::Epub) + 1 == LibraryCounts::kNamedCategories,
                "Article..Epub must stay a dense prefix");

  out = LibraryCounts();
  // A missing or unreadable journal just means no queued overrides. The cached
  // library is still worth counting.
  journal_.load();

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
    if (!decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_, &consumed) || consumed == 0) {
      return false;
    }
    applyQueuedOverrides(scratchDoc_);

    CategoryCounts* bucket = &out.other;
    const auto index = static_cast<uint8_t>(scratchDoc_.category);
    if (index < LibraryCounts::kNamedCategories) {
      bucket = &out.named[index];
    }
    bucket->total++;
    if ((scratchDoc_.flags & FLAG_SEEN) == 0) {
      bucket->unread++;
    }
    if ((scratchDoc_.flags & FLAG_HAS_BODY) != 0) {
      bucket->onDevice++;
    }
    offset += static_cast<uint32_t>(consumed);
  }
  return true;
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

std::string ReadwiseSyncEngine::notePath(const char* id) const { return baseDir_ + "/notes/" + id + ".txt"; }

bool ReadwiseSyncEngine::readNote(const char* id, char* out, size_t outCap) {
  if (out == nullptr || outCap == 0) {
    return false;
  }
  out[0] = '\0';
  if (!isValidDocumentId(id)) {
    return false;
  }
  const std::string path = notePath(id);
  if (!store_.exists(path)) {
    return false;
  }
  const int read = store_.readRange(path, 0, reinterpret_cast<uint8_t*>(out), outCap - 1);
  if (read < 0) {
    out[0] = '\0';
    return false;
  }
  out[read] = '\0';
  return out[0] != '\0';
}

bool ReadwiseSyncEngine::writeNote(const char* id, const char* text) {
  if (!isValidDocumentId(id)) {
    return false;
  }
  const std::string path = notePath(id);
  if (text == nullptr || text[0] == '\0') {
    if (!store_.exists(path)) {
      return true;
    }
    return store_.remove(path);
  }
  if (!store_.ensureDir(baseDir_ + "/notes")) {
    return false;
  }
  char bounded[NOTE_CAP];
  copyBounded(bounded, NOTE_CAP, text, strlen(text));
  return store_.writeAll(path, reinterpret_cast<const uint8_t*>(bounded), strlen(bounded));
}

std::string ReadwiseSyncEngine::highlightDir() const { return baseDir_ + "/highlights"; }

std::string ReadwiseSyncEngine::highlightPath(const char* id) const {
  return highlightDir() + "/" + (id != nullptr ? id : "") + ".bin";
}

std::string ReadwiseSyncEngine::legacyHighlightPath(const char* id) const { return articleDir(id) + "/highlights.bin"; }

bool ReadwiseSyncEngine::relocateHighlight(const char* id) {
  if (!isValidDocumentId(id)) {
    return false;
  }
  const std::string dest = highlightPath(id);
  const std::string legacy = legacyHighlightPath(id);
  if (!store_.exists(legacy)) {
    return true;
  }
  // A crash between the copy and the delete leaves both. The copy is complete
  // (writeAll is atomic), so the leftover is only removed.
  if (store_.exists(dest)) {
    store_.remove(legacy);
    return !store_.exists(legacy);
  }
  const long fileSize = store_.size(legacy);
  if (fileSize < 0 || static_cast<size_t>(fileSize) > HIGHLIGHT_FILE_MAX_BYTES) {
    LOG_ERR("RWSYNC", "Highlight file for %s is missing or too large to move", id);
    return false;
  }
  auto bytes = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(fileSize));
  if (!bytes) {
    LOG_ERR("RWSYNC", "OOM: moving highlight for %s (%ld bytes)", id, fileSize);
    return false;
  }
  if (store_.readRange(legacy, 0, bytes.get(), static_cast<size_t>(fileSize)) != static_cast<int>(fileSize)) {
    LOG_ERR("RWSYNC", "Could not read legacy highlight for %s", id);
    return false;
  }
  if (!store_.ensureDir(highlightDir()) || !store_.writeAll(dest, bytes.get(), static_cast<size_t>(fileSize))) {
    LOG_ERR("RWSYNC", "Could not write highlight for %s", id);
    return false;
  }
  store_.remove(legacy);
  return !store_.exists(legacy);
}

void ReadwiseSyncEngine::dropArticleDir(const char* id) {
  if (id == nullptr || id[0] == '\0') {
    return;
  }
  // Quote files are only created for ids the highlight path accepts. A short
  // test id, or any other body, has nothing to preserve.
  if (isValidDocumentId(id) && (!relocateHighlight(id) || store_.exists(legacyHighlightPath(id)))) {
    LOG_ERR("RWSYNC", "Leaving article dir for %s; an unsent highlight is still in it", id);
    return;
  }
  store_.removeTree(articleDir(id));
}

std::string ReadwiseSyncEngine::highlightIndexPath() const { return baseDir_ + "/highlights.idx"; }

bool ReadwiseSyncEngine::indexHighlight(const char* id) {
  const std::string path = highlightIndexPath();
  uint8_t header[3] = {};
  const int headerRead = store_.readRange(path, 0, header, sizeof(header));
  uint16_t count = 0;
  bool readable = headerRead == static_cast<int>(sizeof(header)) && header[0] == HIGHLIGHT_INDEX_VERSION;
  if (readable) {
    count = static_cast<uint16_t>(header[1] | (header[2] << 8));
    if (count > HIGHLIGHT_INDEX_MAX) {
      readable = false;
      count = 0;
    }
  }
  for (uint16_t i = 0; readable && i < count; ++i) {
    char existing[ID_CAP] = {};
    const size_t at = sizeof(header) + static_cast<size_t>(i) * ID_CAP;
    if (store_.readRange(path, at, reinterpret_cast<uint8_t*>(existing), ID_CAP) != ID_CAP) {
      readable = false;
      count = 0;
      break;
    }
    existing[ID_CAP - 1] = '\0';
    if (sameId(existing, id)) {
      return true;
    }
  }
  if (count >= HIGHLIGHT_INDEX_MAX) {
    return false;
  }
  if (!store_.beginWrite(path)) {
    return false;
  }
  const uint16_t next = static_cast<uint16_t>(count + 1);
  uint8_t nextHeader[3] = {HIGHLIGHT_INDEX_VERSION, static_cast<uint8_t>(next & 0xFF), static_cast<uint8_t>(next >> 8)};
  if (!store_.writeChunk(nextHeader, sizeof(nextHeader))) {
    store_.abortWrite();
    return false;
  }
  for (uint16_t i = 0; readable && i < count; ++i) {
    char existing[ID_CAP] = {};
    const size_t at = sizeof(header) + static_cast<size_t>(i) * ID_CAP;
    if (store_.readRange(path, at, reinterpret_cast<uint8_t*>(existing), ID_CAP) != ID_CAP ||
        !store_.writeChunk(reinterpret_cast<const uint8_t*>(existing), ID_CAP)) {
      store_.abortWrite();
      return false;
    }
  }
  char stored[ID_CAP] = {};
  copyBounded(stored, ID_CAP, id, strlen(id));
  if (!store_.writeChunk(reinterpret_cast<const uint8_t*>(stored), ID_CAP)) {
    store_.abortWrite();
    return false;
  }
  if (!store_.commitWrite()) {
    store_.abortWrite();
    return false;
  }
  return true;
}

bool ReadwiseSyncEngine::appendHighlight(const char* id, const char* text) {
  if (!isValidDocumentId(id) || text == nullptr) {
    return false;
  }
  copyBounded(highlightText_, sizeof(highlightText_), text, strlen(text));
  if (!fitHighlightText(highlightText_, sizeof(highlightText_))) {
    return false;
  }
  // Fold a legacy file in before appending, so a new quote and the old ones
  // stay in one file. A failed move must not start a second file.
  if (!relocateHighlight(id)) {
    return false;
  }
  if (!store_.ensureDir(highlightDir())) {
    return false;
  }
  const std::string path = highlightPath(id);
  const long fileSize = store_.size(path);
  const size_t textLen = strlen(highlightText_);
  if (fileSize < 0) {
    auto created = makeUniqueNoThrow<uint8_t[]>(HIGHLIGHT_HEADER_BYTES + 3 + textLen);
    if (!created) {
      return false;
    }
    memset(created.get(), 0, HIGHLIGHT_HEADER_BYTES);
    created[0] = HIGHLIGHT_FILE_VERSION;
    if (findDocument(id, scratchDoc_)) {
      memcpy(created.get() + 1, scratchDoc_.title, TITLE_CAP);
      memcpy(created.get() + 1 + TITLE_CAP, scratchDoc_.author, AUTHOR_CAP);
      memcpy(created.get() + 1 + TITLE_CAP + AUTHOR_CAP, scratchDoc_.sourceUrl, SOURCE_URL_CAP);
    }
    created[HIGHLIGHT_HEADER_BYTES] = static_cast<uint8_t>(textLen & 0xFF);
    created[HIGHLIGHT_HEADER_BYTES + 1] = static_cast<uint8_t>(textLen >> 8);
    created[HIGHLIGHT_HEADER_BYTES + 2] = 0;
    memcpy(created.get() + HIGHLIGHT_HEADER_BYTES + 3, highlightText_, textLen);
    if (!store_.writeAll(path, created.get(), HIGHLIGHT_HEADER_BYTES + 3 + textLen)) {
      return false;
    }
    return indexHighlight(id);
  }
  if (static_cast<size_t>(fileSize) < HIGHLIGHT_HEADER_BYTES) {
    return false;
  }
  uint8_t version = 0;
  if (store_.readRange(path, 0, &version, 1) != 1 || version != HIGHLIGHT_FILE_VERSION) {
    return false;
  }
  // The scan reads each stored quote into highlightText_, so the fitted text
  // has to live somewhere else. 281 bytes is over the stack budget.
  auto fitted = makeUniqueNoThrow<char[]>(textLen + 1);
  if (!fitted) {
    return false;
  }
  memcpy(fitted.get(), highlightText_, textLen + 1);

  size_t offset = HIGHLIGHT_HEADER_BYTES;
  int records = 0;
  while (offset + 3 <= static_cast<size_t>(fileSize)) {
    uint8_t prefix[3] = {};
    if (store_.readRange(path, offset, prefix, sizeof(prefix)) != static_cast<int>(sizeof(prefix))) {
      return false;
    }
    const size_t len = static_cast<size_t>(prefix[0] | (prefix[1] << 8));
    if (len == 0 || len > HIGHLIGHT_TEXT_MAX || offset + 3 + len > static_cast<size_t>(fileSize)) {
      return false;
    }
    if (store_.readRange(path, offset + 3, reinterpret_cast<uint8_t*>(highlightText_), len) != static_cast<int>(len)) {
      return false;
    }
    highlightText_[len] = '\0';
    if (strcmp(highlightText_, fitted.get()) == 0) {
      return indexHighlight(id);
    }
    offset += 3 + len;
    ++records;
  }
  if (records >= HIGHLIGHT_MAX_PER_DOC) {
    return false;
  }
  if (!store_.beginWrite(path)) {
    return false;
  }
  size_t copied = 0;
  uint8_t chunk[128];
  while (copied < static_cast<size_t>(fileSize)) {
    const size_t want =
        static_cast<size_t>(fileSize) - copied < sizeof(chunk) ? static_cast<size_t>(fileSize) - copied : sizeof(chunk);
    const int got = store_.readRange(path, copied, chunk, want);
    if (got <= 0 || !store_.writeChunk(chunk, static_cast<size_t>(got))) {
      store_.abortWrite();
      return false;
    }
    copied += static_cast<size_t>(got);
  }
  uint8_t recordHead[3] = {static_cast<uint8_t>(textLen & 0xFF), static_cast<uint8_t>(textLen >> 8), 0};
  if (!store_.writeChunk(recordHead, sizeof(recordHead)) ||
      !store_.writeChunk(reinterpret_cast<const uint8_t*>(fitted.get()), textLen) || !store_.commitWrite()) {
    store_.abortWrite();
    return false;
  }
  return indexHighlight(id);
}

int ReadwiseSyncEngine::readHighlights(const char* id, StoredHighlight* out, const int cap) {
  if (out == nullptr || cap <= 0 || !isValidDocumentId(id)) {
    return 0;
  }
  relocateHighlight(id);
  const std::string path = highlightPath(id);
  const long fileSize = store_.size(path);
  if (fileSize < 0 || static_cast<size_t>(fileSize) < HIGHLIGHT_HEADER_BYTES) {
    return 0;
  }
  uint8_t version = 0;
  if (store_.readRange(path, 0, &version, 1) != 1 || version != HIGHLIGHT_FILE_VERSION) {
    return 0;
  }
  size_t offset = HIGHLIGHT_HEADER_BYTES;
  int count = 0;
  while (offset + 3 <= static_cast<size_t>(fileSize) && count < cap) {
    uint8_t prefix[3] = {};
    if (store_.readRange(path, offset, prefix, sizeof(prefix)) != static_cast<int>(sizeof(prefix))) {
      break;
    }
    const size_t len = static_cast<size_t>(prefix[0] | (prefix[1] << 8));
    if (len == 0 || len > HIGHLIGHT_TEXT_MAX || offset + 3 + len > static_cast<size_t>(fileSize)) {
      break;
    }
    if (store_.readRange(path, offset + 3, reinterpret_cast<uint8_t*>(out[count].text), len) != static_cast<int>(len)) {
      break;
    }
    out[count].text[len] = '\0';
    out[count].flags = prefix[2];
    ++count;
    offset += 3 + len;
  }
  return count;
}

ApiStatus ReadwiseSyncEngine::pushHighlightFile(const char* id, bool& stillPending, char* step, size_t stepCap,
                                                HighlightPushStats& stats) {
  stillPending = false;
  relocateHighlight(id);
  const std::string path = highlightPath(id);
  const long fileSize = store_.size(path);
  if (fileSize < 0) {
    return ApiStatus::Ok;
  }
  if (static_cast<size_t>(fileSize) < HIGHLIGHT_HEADER_BYTES) {
    stillPending = true;
    return ApiStatus::Ok;
  }
  uint8_t version = 0;
  if (store_.readRange(path, 0, &version, 1) != 1 || version != HIGHLIGHT_FILE_VERSION) {
    stillPending = true;
    return ApiStatus::Ok;
  }
  if (store_.readRange(path, 1, reinterpret_cast<uint8_t*>(scratchDoc_.title), TITLE_CAP) != TITLE_CAP ||
      store_.readRange(path, 1 + TITLE_CAP, reinterpret_cast<uint8_t*>(scratchDoc_.author), AUTHOR_CAP) != AUTHOR_CAP ||
      store_.readRange(path, 1 + TITLE_CAP + AUTHOR_CAP, reinterpret_cast<uint8_t*>(scratchDoc_.sourceUrl),
                       SOURCE_URL_CAP) != SOURCE_URL_CAP) {
    stillPending = true;
    return ApiStatus::Ok;
  }
  scratchDoc_.title[TITLE_CAP - 1] = '\0';
  scratchDoc_.author[AUTHOR_CAP - 1] = '\0';
  scratchDoc_.sourceUrl[SOURCE_URL_CAP - 1] = '\0';

  size_t offset = HIGHLIGHT_HEADER_BYTES;
  while (offset + 3 <= static_cast<size_t>(fileSize)) {
    uint8_t prefix[3] = {};
    if (store_.readRange(path, offset, prefix, sizeof(prefix)) != static_cast<int>(sizeof(prefix))) {
      stillPending = true;
      return ApiStatus::Ok;
    }
    const size_t len = static_cast<size_t>(prefix[0] | (prefix[1] << 8));
    if (len == 0 || len > HIGHLIGHT_TEXT_MAX || offset + 3 + len > static_cast<size_t>(fileSize)) {
      stillPending = true;
      return ApiStatus::Ok;
    }
    const uint8_t flags = prefix[2];
    const bool settled = (flags & (HIGHLIGHT_FLAG_POSTED | HIGHLIGHT_FLAG_FAILED)) != 0;
    if (!settled) {
      if (store_.readRange(path, offset + 3, reinterpret_cast<uint8_t*>(highlightText_), len) !=
          static_cast<int>(len)) {
        stillPending = true;
        return ApiStatus::Ok;
      }
      highlightText_[len] = '\0';
      ApiStatus status =
          api_.pushHighlight(highlightText_, scratchDoc_.title, scratchDoc_.author, scratchDoc_.sourceUrl);
      if (status == ApiStatus::RateLimited) {
        for (int retry = 0; retry < MAX_RATE_LIMIT_RETRIES && status == ApiStatus::RateLimited; ++retry) {
          status = api_.pushHighlight(highlightText_, scratchDoc_.title, scratchDoc_.author, scratchDoc_.sourceUrl);
        }
      }
      // 4xx (and a highlight 404) will not succeed on retry. Mark the quote and
      // keep going so one bad clip cannot stall every later sync.
      if (status == ApiStatus::Rejected || status == ApiStatus::NotFound) {
        const uint8_t failed = static_cast<uint8_t>(flags | HIGHLIGHT_FLAG_FAILED);
        if (!store_.writeRange(path, offset + 2, &failed, 1)) {
          stillPending = true;
          return ApiStatus::Ok;
        }
        if (stats.failed < 0xFFFF) {
          ++stats.failed;
        }
        const char* http = api_.lastDetail();
        LOG_ERR("RWSYNC", "Highlight rejected for %s: %s", id != nullptr ? id : "",
                http != nullptr && http[0] != '\0' ? http : apiStatusName(status));
        if (stats.detail[0] == '\0') {
          if (http != nullptr && http[0] != '\0') {
            snprintf(stats.detail, sizeof(stats.detail), "highlight %s: %s", id != nullptr ? id : "", http);
          } else {
            snprintf(stats.detail, sizeof(stats.detail), "highlight %s: %s", id != nullptr ? id : "",
                     apiStatusName(status));
          }
        }
      } else if (status != ApiStatus::Ok) {
        stillPending = true;
        if (step != nullptr && stepCap > 0) {
          snprintf(step, stepCap, "highlight %s", id != nullptr ? id : "");
        }
        return status;
      } else {
        const uint8_t posted = static_cast<uint8_t>(flags | HIGHLIGHT_FLAG_POSTED);
        if (!store_.writeRange(path, offset + 2, &posted, 1)) {
          stillPending = true;
          return ApiStatus::Ok;
        }
        if (stats.sent < 0xFFFF) {
          ++stats.sent;
        }
      }
    }
    offset += 3 + len;
  }
  return ApiStatus::Ok;
}

ApiStatus ReadwiseSyncEngine::pushPendingHighlights(char* step, size_t stepCap, HighlightPushStats& stats) {
  const std::string path = highlightIndexPath();
  uint8_t header[3] = {};
  if (store_.readRange(path, 0, header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      header[0] != HIGHLIGHT_INDEX_VERSION) {
    return ApiStatus::Ok;
  }
  const uint16_t count = static_cast<uint16_t>(header[1] | (header[2] << 8));
  if (count == 0 || count > HIGHLIGHT_INDEX_MAX) {
    return ApiStatus::Ok;
  }
  if (!store_.beginWrite(path)) {
    writeStep(step, stepCap, "highlight index");
    return ApiStatus::ServerError;
  }
  uint8_t placeholder[3] = {HIGHLIGHT_INDEX_VERSION, 0, 0};
  if (!store_.writeChunk(placeholder, sizeof(placeholder))) {
    store_.abortWrite();
    writeStep(step, stepCap, "highlight index");
    return ApiStatus::ServerError;
  }
  uint16_t kept = 0;
  for (uint16_t i = 0; i < count; ++i) {
    char id[ID_CAP] = {};
    const size_t at = sizeof(header) + static_cast<size_t>(i) * ID_CAP;
    if (store_.readRange(path, at, reinterpret_cast<uint8_t*>(id), ID_CAP) != ID_CAP) {
      store_.abortWrite();
      writeStep(step, stepCap, "highlight index");
      return ApiStatus::ServerError;
    }
    id[ID_CAP - 1] = '\0';
    bool stillPending = false;
    const ApiStatus status = pushHighlightFile(id, stillPending, step, stepCap, stats);
    if (status != ApiStatus::Ok) {
      store_.abortWrite();
      return status;
    }
    if (!stillPending) {
      continue;
    }
    if (!store_.writeChunk(reinterpret_cast<const uint8_t*>(id), ID_CAP)) {
      store_.abortWrite();
      writeStep(step, stepCap, "highlight index");
      return ApiStatus::ServerError;
    }
    ++kept;
  }
  uint8_t nextHeader[3] = {HIGHLIGHT_INDEX_VERSION, static_cast<uint8_t>(kept & 0xFF), static_cast<uint8_t>(kept >> 8)};
  if (!store_.patchWrite(0, nextHeader, sizeof(nextHeader)) || !store_.commitWrite()) {
    store_.abortWrite();
    writeStep(step, stepCap, "highlight index");
    return ApiStatus::ServerError;
  }
  return ApiStatus::Ok;
}

bool ReadwiseSyncEngine::forgetDocument(const char* id) {
  if (!isValidDocumentId(id)) {
    return false;
  }
  copyBounded(droppingId_, ID_CAP, id, strlen(id));
  dropArticleDir(id);
  writeNote(id, "");
  std::vector<IndexEntry> indexEntries;
  indexEntries.reserve(static_cast<size_t>(documentCap_) + feedCap_);
  uint16_t retained = 0;
  const bool merged =
      mergeIntoDocs(docsPath(), {}, /*carryOverExisting=*/true, /*dropExpired=*/false, indexEntries, retained);
  droppingId_[0] = '\0';
  if (!merged) {
    return false;
  }
  return writeIndexes(indexEntries);
}

bool ReadwiseSyncEngine::readIndexBounds(const Location location, uint16_t& total, DocsHeader& header) {
  uint8_t indexHeader[INDEX_HEADER_SIZE];
  if (store_.readRange(indexPath(location), 0, indexHeader, INDEX_HEADER_SIZE) != static_cast<int>(INDEX_HEADER_SIZE) ||
      !decodeIndexHeader(indexHeader, INDEX_HEADER_SIZE, total)) {
    return false;
  }
  uint8_t docsHeaderBuffer[DOCS_HEADER_SIZE];
  if (store_.readRange(docsPath(), 0, docsHeaderBuffer, DOCS_HEADER_SIZE) != static_cast<int>(DOCS_HEADER_SIZE) ||
      !decodeDocsHeader(docsHeaderBuffer, DOCS_HEADER_SIZE, header)) {
    return false;
  }
  return true;
}

bool ReadwiseSyncEngine::loadRecord(const uint16_t recordIndex, const DocsHeader& header) {
  if (recordIndex >= header.recordCount) {
    return false;
  }
  uint8_t lutEntry[4];
  const size_t lutOffset = header.lutOffset + static_cast<size_t>(recordIndex) * 4;
  if (store_.readRange(docsPath(), lutOffset, lutEntry, 4) != 4) {
    return false;
  }
  const uint32_t recordOffset = static_cast<uint32_t>(lutEntry[0]) | (static_cast<uint32_t>(lutEntry[1]) << 8) |
                                (static_cast<uint32_t>(lutEntry[2]) << 16) | (static_cast<uint32_t>(lutEntry[3]) << 24);
  const int read = store_.readRange(docsPath(), recordOffset, recordBuffer_, MAX_ENCODED_RECORD);
  return read > 0 && decodeDocument(recordBuffer_, static_cast<size_t>(read), scratchDoc_);
}

bool ReadwiseSyncEngine::loadIndexedDocument(const Location location, const uint16_t slot, const DocsHeader& header,
                                             uint16_t* recordIndexOut) {
  uint8_t entry[INDEX_ENTRY_SIZE];
  const size_t entryOffset = INDEX_HEADER_SIZE + static_cast<size_t>(slot) * INDEX_ENTRY_SIZE;
  if (store_.readRange(indexPath(location), entryOffset, entry, INDEX_ENTRY_SIZE) !=
      static_cast<int>(INDEX_ENTRY_SIZE)) {
    return false;
  }
  const uint16_t recordIndex = decodeIndexEntry(entry);
  if (recordIndexOut != nullptr) {
    *recordIndexOut = recordIndex;
  }
  return loadRecord(recordIndex, header);
}

bool ReadwiseSyncEngine::collectAuthorSlots(const Location location, const char* author, std::vector<uint16_t>& out) {
  out.clear();
  if (author == nullptr || author[0] == '\0') {
    return false;
  }
  uint16_t total = 0;
  DocsHeader header;
  if (!readIndexBounds(location, total, header)) {
    return indexCount(location) == 0;
  }
  out.reserve(total);
  for (uint16_t i = 0; i < total; ++i) {
    if (!loadIndexedDocument(location, i, header)) {
      return false;
    }
    if (strncmp(scratchDoc_.author, author, AUTHOR_CAP) == 0) {
      out.push_back(i);
    }
  }
  return true;
}

bool ReadwiseSyncEngine::collectLengthSlots(const uint32_t minWords, const bool longReads, const char* author,
                                            std::vector<uint16_t>& out) {
  out.clear();
  // lastMovedAt is the merge key across the two indexes and is only on the
  // record. The dates live for this scan; the caller keeps the record numbers.
  struct Hit {
    uint16_t recordIndex;
    char lastMovedAt[TIMESTAMP_CAP];
  };
  std::vector<Hit> hits;
  hits.reserve(documentCap_);

  const Location sources[] = {Location::Later, Location::Shortlist};
  for (const Location location : sources) {
    uint16_t total = 0;
    DocsHeader header;
    if (!readIndexBounds(location, total, header)) {
      if (indexCount(location) == 0) {
        continue;
      }
      return false;
    }
    for (uint16_t i = 0; i < total; ++i) {
      uint16_t recordIndex = 0;
      if (!loadIndexedDocument(location, i, header, &recordIndex)) {
        out.clear();
        return false;
      }
      const uint32_t words = scratchDoc_.wordCount;
      const bool onSide = longReads ? (words >= minWords) : (words > 0 && words < minWords);
      if (!onSide) {
        continue;
      }
      if (author != nullptr && author[0] != '\0' && strncmp(scratchDoc_.author, author, AUTHOR_CAP) != 0) {
        continue;
      }
      Hit hit;
      hit.recordIndex = recordIndex;
      copyBounded(hit.lastMovedAt, TIMESTAMP_CAP, scratchDoc_.lastMovedAt, strlen(scratchDoc_.lastMovedAt));
      hits.push_back(hit);
    }
  }

  std::sort(hits.begin(), hits.end(),
            [](const Hit& a, const Hit& b) { return strncmp(a.lastMovedAt, b.lastMovedAt, TIMESTAMP_CAP) > 0; });
  out.reserve(hits.size());
  for (const Hit& hit : hits) {
    out.push_back(hit.recordIndex);
  }
  return true;
}

bool ReadwiseSyncEngine::readRecords(const uint16_t* recordIndexes, const uint16_t count, std::vector<Document>& out) {
  out.clear();
  if (count == 0) {
    return true;
  }
  if (recordIndexes == nullptr) {
    return false;
  }
  DocsHeader header;
  uint8_t docsHeaderBuffer[DOCS_HEADER_SIZE];
  if (store_.readRange(docsPath(), 0, docsHeaderBuffer, DOCS_HEADER_SIZE) != static_cast<int>(DOCS_HEADER_SIZE) ||
      !decodeDocsHeader(docsHeaderBuffer, DOCS_HEADER_SIZE, header)) {
    return false;
  }
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    if (!loadRecord(recordIndexes[i], header)) {
      out.clear();
      return false;
    }
    out.push_back(scratchDoc_);
  }
  return true;
}

bool ReadwiseSyncEngine::readIndexSlots(const Location location, const uint16_t* slots, const uint16_t slotCount,
                                        std::vector<Document>& out) {
  out.clear();
  if (slotCount == 0) {
    return true;
  }
  if (slots == nullptr) {
    return false;
  }
  uint16_t total = 0;
  DocsHeader header;
  if (!readIndexBounds(location, total, header)) {
    return false;
  }
  out.reserve(slotCount);
  for (uint16_t i = 0; i < slotCount; ++i) {
    if (slots[i] >= total || !loadIndexedDocument(location, slots[i], header)) {
      return false;
    }
    out.push_back(scratchDoc_);
  }
  return true;
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

  // Carries the current article into the image-progress function pointer.
  // Title points at scratchDoc_, which stays put for this iteration.
  struct StepSink {
    const BodySyncHooks* hooks = nullptr;
    BodySyncProgress progress{};

    void emit(BodySyncStep step, uint16_t index, uint16_t count) {
      if (hooks == nullptr || hooks->onStep == nullptr) {
        return;
      }
      progress.step = step;
      progress.index = index;
      progress.count = count;
      hooks->onStep(hooks->ctx, progress);
    }
  };

  uint16_t done = 0;
  for (const MissingId& entry : missing) {
    ApiStatus status = ApiStatus::NetworkError;
    // Relative image srcs resolve against source_url, and the OPF wants the
    // title and author, so the record is re-read rather than carried through
    // the scan (which only kept ids, to bound the missing list).
    const bool haveDoc = findDocument(entry.id, scratchDoc_);
    store_.ensureDir(articleDir(entry.id));
    const std::string xhtmlPath = articleDir(entry.id) + "/.body.xhtml";
    const std::string scratchPath = articleDir(entry.id) + "/.img.tmp";

    StepSink sink;
    sink.hooks = &hooks;
    sink.progress.articlesDone = done;
    sink.progress.articlesTotal = outcome.total;
    sink.progress.title = haveDoc ? scratchDoc_.title : "";
    sink.progress.wordCount = haveDoc ? scratchDoc_.wordCount : 0;
    sink.progress.category = haveDoc ? scratchDoc_.category : Category::Unknown;

    for (int attempt = 0; attempt < 1 + MAX_RATE_LIMIT_RETRIES; ++attempt) {
      auto writer = makeUniqueNoThrow<ArticleBodyWriter>(store_, xhtmlPath, haveDoc ? scratchDoc_.sourceUrl : "",
                                                         haveDoc ? scratchDoc_.title : "");
      if (!writer) {
        status = ApiStatus::LowMemory;
        break;
      }
      uint16_t retryAfter = 0;
      // The HTML fetch is the long silent stretch: every tag is converted as
      // it arrives, and the image list is only known once that finishes.
      sink.emit(BodySyncStep::Article, 0, 0);
      status = api_.fetchBody(entry.id, *writer, &retryAfter);
      if (status == ApiStatus::Ok && !writer->committed()) {
        // 200 with no html_content string: nothing to retry.
        status = ApiStatus::ParseError;
      }
      if (status == ApiStatus::Ok) {
        // Images are fetched here, after the body's TLS session has closed --
        // a nested request inside the read callback is impossible, and this is
        // also the heap's worst moment. A failure to assemble is a failure to
        // cache; a failure to fetch any individual image is not.
        NullArticleImageFetcher noImages;
        ArticleImageFetcher& fetcher =
            hooks.imageFetcher != nullptr ? *hooks.imageFetcher : static_cast<ArticleImageFetcher&>(noImages);
        const ArticleAssemblyResult assembly = assembleArticleEpub(
            store_, fetcher, *writer, xhtmlPath, scratchPath, bodyPath(entry.id), haveDoc ? scratchDoc_.title : "",
            haveDoc ? scratchDoc_.author : "",
            [](void* raw, size_t index, size_t total) {
              // The trailing (count, count) call repeats the last image.
              if (raw == nullptr || total == 0 || index >= total) {
                return;
              }
              static_cast<StepSink*>(raw)->emit(BodySyncStep::Image, static_cast<uint16_t>(index),
                                                static_cast<uint16_t>(total));
            },
            &sink);
        if (!assembly.ok) {
          store_.remove(xhtmlPath);
          store_.remove(scratchPath);
          status = ApiStatus::ParseError;
        }
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
        sink.emit(BodySyncStep::RateLimit, 0, 0);
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
      if (outcome.failed == 0) {
        outcome.failedStatus = status;
        if (haveDoc) {
          copyBounded(outcome.failedTitle, TITLE_CAP, scratchDoc_.title, strlen(scratchDoc_.title));
        }
      }
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
      dropArticleDir(scratchDoc_.id);
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
