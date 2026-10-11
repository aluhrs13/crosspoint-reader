// Covers the sync pipeline's durability and conflict behaviour, which is what
// issue #3's acceptance criteria are actually about.
//
// Everything runs against in-memory fakes, so a write can be failed at an exact
// commit boundary -- the interruption case that cannot be produced reliably on
// real hardware.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "FakeReadwise.h"
#include "lib/Readwise/ReadwiseSyncEngine.h"

namespace {

using namespace readwise;
using testing_support::FakeApi;
using testing_support::FakeFileStore;
using testing_support::makeDoc;

constexpr const char* kBase = "/.crosspoint/readwise";

const char* kT1 = "2026-08-01T00:00:00.000000+00:00";
const char* kT2 = "2026-08-02T00:00:00.000000+00:00";
const char* kT3 = "2026-08-03T00:00:00.000000+00:00";

struct Fixture {
  FakeFileStore store;
  FakeApi api;
  ReadwiseSyncEngine engine{api, store, kBase};
};

}  // namespace

TEST(ReadwiseSync, SyncWritesDocsIndexesAndCheckpoint) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});

  const SyncOutcome outcome = f.engine.sync();
  ASSERT_TRUE(outcome.ok) << "failed at stage " << static_cast<int>(outcome.failedStage);
  EXPECT_EQ(outcome.pulled, 2);
  EXPECT_EQ(outcome.retained, 2);

  EXPECT_TRUE(f.store.has(f.engine.docsPath()));
  EXPECT_TRUE(f.store.has(f.engine.indexPath(Location::Later)));
  EXPECT_TRUE(f.store.has(f.engine.checkpointPath()));

  Checkpoint checkpoint;
  ASSERT_TRUE(f.engine.loadCheckpoint(checkpoint));
  // The cursor is the highest updated_at actually observed, never a device clock.
  EXPECT_STREQ(checkpoint.updatedAfter, kT2);
  EXPECT_EQ(checkpoint.docCount, 2);
}

// A page can be read without touching the rest of the index or the other
// records, which is the bounded-memory requirement.
TEST(ReadwiseSync, IndexPagesAreOrderedNewestFirstAndBounded) {
  Fixture f;
  f.api.pages.push_back({{makeDoc("old", Location::Later, kT1, kT1), makeDoc("new", Location::Later, kT3, kT3),
                          makeDoc("mid", Location::Later, kT2, kT2)},
                         "",
                         ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 2, page));
  ASSERT_EQ(page.size(), 2u);
  EXPECT_STREQ(page[0].id, "new");
  EXPECT_STREQ(page[1].id, "mid");

  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 2, 2, page));
  ASSERT_EQ(page.size(), 1u) << "a page past the end must clamp, not fail";
  EXPECT_STREQ(page[0].id, "old");
}

TEST(ReadwiseSync, DocumentCapIsEnforced) {
  Fixture f;
  f.engine.setDocumentCap(2);
  f.api.pages.push_back({{makeDoc("a", Location::Later, kT1, kT1), makeDoc("b", Location::Later, kT2, kT2),
                          makeDoc("c", Location::Later, kT3, kT3)},
                         "",
                         ApiStatus::Ok});

  const SyncOutcome outcome = f.engine.sync();
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.retained, 2);
}

// --- durability -----------------------------------------------------------

// The checkpoint is written last, so a failure before it must leave the previous
// checkpoint in place. Resuming then re-pulls the same window, which costs
// duplicates rather than losing documents.
TEST(ReadwiseSync, FailureBeforeCommitPreservesPreviousCheckpoint) {
  Fixture f;
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  Checkpoint before;
  ASSERT_TRUE(f.engine.loadCheckpoint(before));

  // Second sync: fail the very last durable write, which is the checkpoint.
  // Durable writes per sync: staging commit, docs.bin commit, three index
  // writeAlls (Later, Shortlist, Feed), then the checkpoint.
  f.api.pages.push_back({{makeDoc("doc2", Location::Later, kT3, kT3)}, "", ApiStatus::Ok});
  const int writesBefore = f.store.writeCount();
  f.store.failAtWrite(writesBefore + 6);

  f.engine.sync();

  Checkpoint after;
  ASSERT_TRUE(f.engine.loadCheckpoint(after));
  EXPECT_STREQ(after.updatedAfter, before.updatedAfter) << "an interrupted sync must not advance the committed cursor";
}

TEST(ReadwiseSync, NetworkFailureDuringPullLeavesNoCheckpoint) {
  Fixture f;
  f.api.pages.push_back({{}, "", ApiStatus::NetworkError});

  const SyncOutcome outcome = f.engine.sync();
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.failedStage, SyncStage::Pulling);
  EXPECT_EQ(outcome.status, ApiStatus::NetworkError);
  EXPECT_STREQ(outcome.detail, "pull later");
  EXPECT_FALSE(f.store.has(f.engine.checkpointPath()));
}

// A rate-limited page is retried exactly once, then the pass is abandoned. A
// blocking retry loop could exceed the watchdog window.
TEST(ReadwiseSync, RateLimitIsRetriedExactlyOnce) {
  Fixture f;
  f.api.pages.push_back({{}, "", ApiStatus::RateLimited});
  f.api.pages.push_back({{}, "", ApiStatus::RateLimited});
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT1, kT1)}, "", ApiStatus::Ok});

  const SyncOutcome outcome = f.engine.sync();
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.status, ApiStatus::RateLimited);
  EXPECT_EQ(f.api.pageIndex, 2u) << "expected one initial attempt plus exactly one retry";
}

// --- journal --------------------------------------------------------------

TEST(ReadwiseJournalTest, QueuedOpsSurviveReload) {
  FakeFileStore store;
  {
    ReadwiseJournal journal(store, std::string(kBase) + "/journal.bin");
    ASSERT_TRUE(journal.load());
    ASSERT_TRUE(journal.append(OpType::SetLocation, "doc1", static_cast<uint8_t>(Location::Archive), kT1));
    ASSERT_TRUE(journal.append(OpType::SetSeen, "doc2", 1, kT2));
  }

  ReadwiseJournal reloaded(store, std::string(kBase) + "/journal.bin");
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.entries().size(), 2u);
  EXPECT_STREQ(reloaded.entries()[0].id, "doc1");
  EXPECT_EQ(reloaded.entries()[1].op, OpType::SetSeen);
}

// Fixed-width entries exist so an interrupted append costs only the record being
// written; everything before it must still replay.
TEST(ReadwiseJournalTest, TruncatedTailIsDiscardedNotFatal) {
  FakeFileStore store;
  const std::string path = std::string(kBase) + "/journal.bin";
  {
    ReadwiseJournal journal(store, path);
    ASSERT_TRUE(journal.load());
    ASSERT_TRUE(journal.append(OpType::SetLocation, "doc1", 1, kT1));
    ASSERT_TRUE(journal.append(OpType::SetLocation, "doc2", 2, kT2));
  }

  // Chop the last entry in half.
  std::vector<uint8_t> data = store.files().at(path);
  data.resize(data.size() - JOURNAL_ENTRY_SIZE / 2);
  store.put(path, data);

  ReadwiseJournal reloaded(store, path);
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.entries().size(), 1u) << "the intact entry must survive a torn append";
  EXPECT_STREQ(reloaded.entries()[0].id, "doc1");
}

TEST(ReadwiseJournalTest, CoalesceKeepsNewestPerDocumentAndOp) {
  FakeFileStore store;
  ReadwiseJournal journal(store, std::string(kBase) + "/journal.bin");
  ASSERT_TRUE(journal.load());
  ASSERT_TRUE(journal.append(OpType::SetLocation, "doc1", static_cast<uint8_t>(Location::Later), kT1));
  ASSERT_TRUE(journal.append(OpType::SetLocation, "doc2", static_cast<uint8_t>(Location::Later), kT1));
  ASSERT_TRUE(journal.append(OpType::SetLocation, "doc1", static_cast<uint8_t>(Location::Archive), kT2));
  ASSERT_TRUE(journal.append(OpType::SetSeen, "doc1", 1, kT2));

  journal.coalesce();

  ASSERT_EQ(journal.entries().size(), 3u);
  // doc2's op predates doc1's surviving op, so relative order is preserved.
  EXPECT_STREQ(journal.entries()[0].id, "doc2");
  EXPECT_EQ(journal.entries()[1].payload, static_cast<uint8_t>(Location::Archive));
  EXPECT_EQ(journal.entries()[2].op, OpType::SetSeen)
      << "a different op on the same document must not be coalesced away";
}

TEST(ReadwiseJournalTest, AcknowledgedOpsRemovedWithoutLosingLaterOnes) {
  FakeFileStore store;
  const std::string path = std::string(kBase) + "/journal.bin";
  ReadwiseJournal journal(store, path);
  ASSERT_TRUE(journal.load());
  ASSERT_TRUE(journal.append(OpType::SetLocation, "doc1", 1, kT1));
  ASSERT_TRUE(journal.append(OpType::SetLocation, "doc2", 1, kT1));
  ASSERT_TRUE(journal.append(OpType::SetLocation, "doc3", 1, kT1));

  const uint32_t firstSeq = journal.entries()[0].seq;
  ASSERT_TRUE(journal.removeAcknowledged({firstSeq}));

  ReadwiseJournal reloaded(store, path);
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.entries().size(), 2u);
  EXPECT_STREQ(reloaded.entries()[0].id, "doc2");
  EXPECT_STREQ(reloaded.entries()[1].id, "doc3");
}

struct NotFoundAsk {
  bool drop = false;
  int calls = 0;
  const char* id = "doc2";
  bool expectTitle = false;
};

bool answerNotFound(void* ctx, const PendingOp& op, const char* title, const char* detail) {
  auto* ask = static_cast<NotFoundAsk*>(ctx);
  ++ask->calls;
  EXPECT_STREQ(op.id, ask->id);
  EXPECT_NE(std::string(detail != nullptr ? detail : "").find("HTTP 404"), std::string::npos);
  if (ask->expectTitle) {
    EXPECT_STREQ(title, "Title");
  }
  return ask->drop;
}

// A push failure mid-queue must keep the unacknowledged operations, so nothing
// the user asked for is silently dropped.
TEST(ReadwiseSync, FailedPushPreservesUnacknowledgedOps) {
  Fixture f;
  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::Archive, kT1));
  ASSERT_TRUE(f.engine.queueLocationChange("doc2", Location::Archive, kT1));
  ASSERT_TRUE(f.engine.queueLocationChange("doc3", Location::Archive, kT1));
  f.api.failPushAt = 2;
  f.api.pushStatus = ApiStatus::ServerError;
  f.api.detail = "HTTP 400 {\"detail\":\"no\"}";

  const SyncOutcome outcome = f.engine.sync();
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.failedStage, SyncStage::Pushing);
  EXPECT_EQ(outcome.status, ApiStatus::ServerError);
  EXPECT_EQ(outcome.pushed, 1);
  EXPECT_NE(std::string(outcome.detail).find("move archive doc2"), std::string::npos) << outcome.detail;
  EXPECT_NE(std::string(outcome.detail).find("HTTP 400"), std::string::npos);

  ReadwiseJournal reloaded(f.store, f.engine.journalPath());
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.entries().size(), 2u) << "the acknowledged op is dropped, the rest are kept";
  EXPECT_STREQ(reloaded.entries()[0].id, "doc2");
}

// A 404 with nobody to ask aborts like any other push failure and keeps the op,
// so a missing prompt cannot silently throw away a queued archive.
TEST(ReadwiseSync, NotFoundWithoutHookAbortsAndKeepsTheOp) {
  Fixture f;
  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::Archive, kT1));
  ASSERT_TRUE(f.engine.queueLocationChange("doc2", Location::Archive, kT1));
  f.api.failPushAt = 2;
  f.api.pushStatus = ApiStatus::NotFound;
  f.api.detail = "HTTP 404";

  const SyncOutcome outcome = f.engine.sync();
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.failedStage, SyncStage::Pushing);
  EXPECT_EQ(outcome.status, ApiStatus::NotFound);
  EXPECT_EQ(outcome.pushed, 1);
  EXPECT_NE(std::string(outcome.detail).find("move archive doc2"), std::string::npos) << outcome.detail;
  EXPECT_NE(std::string(outcome.detail).find("HTTP 404"), std::string::npos);

  ReadwiseJournal reloaded(f.store, f.engine.journalPath());
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.entries().size(), 1u);
  EXPECT_STREQ(reloaded.entries()[0].id, "doc2");
}

// Stop leaves the op queued. The next sync will ask again.
TEST(ReadwiseSync, NotFoundStopKeepsTheOp) {
  Fixture f;
  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::Archive, kT1));
  f.api.failPushAt = 1;
  f.api.pushStatus = ApiStatus::NotFound;
  f.api.detail = "HTTP 404";

  NotFoundAsk ask;
  ask.id = "doc1";
  ask.drop = false;
  f.engine.setNotFoundHooks(ReadwiseSyncEngine::NotFoundHooks{&ask, answerNotFound});

  const SyncOutcome outcome = f.engine.sync();
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.status, ApiStatus::NotFound);
  EXPECT_EQ(ask.calls, 1);
  EXPECT_EQ(outcome.pushed, 0);

  ReadwiseJournal reloaded(f.store, f.engine.journalPath());
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.entries().size(), 1u);
  EXPECT_STREQ(reloaded.entries()[0].id, "doc1");
}

// Continue removes the missing document and its queued op, then the rest of
// the sync still runs. The drop is persisted before the pull, so a later
// failure does not bring the 404 back on the next run.
TEST(ReadwiseSync, NotFoundContinueDropsTheOpAndContinues) {
  Fixture f;
  // Ids have to be long enough for forgetDocument; short test ids are refused.
  constexpr const char* missing = "01hzzzzzzzzzzzzzzzzzzzzz01";
  constexpr const char* kept = "01hzzzzzzzzzzzzzzzzzzzzz02";
  f.api.pages.push_back(
      {{makeDoc(missing, Location::Later, kT1, kT1), makeDoc(kept, Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  ASSERT_TRUE(f.engine.queueLocationChange(missing, Location::Archive, kT1));
  ASSERT_TRUE(f.engine.queueLocationChange(kept, Location::Archive, kT1));
  f.api.failPushAt = 1;
  f.api.pushStatus = ApiStatus::NotFound;
  f.api.detail = "HTTP 404 {\"detail\":\"not found\"}";

  NotFoundAsk ask;
  ask.id = missing;
  ask.drop = true;
  ask.expectTitle = true;
  f.engine.setNotFoundHooks(ReadwiseSyncEngine::NotFoundHooks{&ask, answerNotFound});

  const SyncOutcome outcome = f.engine.sync();
  ASSERT_TRUE(outcome.ok) << outcome.detail;
  EXPECT_EQ(ask.calls, 1);
  EXPECT_EQ(outcome.pushed, 1);
  ASSERT_EQ(f.api.pushed.size(), 1u);
  EXPECT_STREQ(f.api.pushed[0].id, kept);

  ReadwiseJournal reloaded(f.store, f.engine.journalPath());
  ASSERT_TRUE(reloaded.load());
  EXPECT_TRUE(reloaded.entries().empty());

  Document gone;
  EXPECT_FALSE(f.engine.findDocument(missing, gone));
}

TEST(ReadwiseSync, NotFoundContinueStaysDroppedWhenPullFails) {
  Fixture f;
  constexpr const char* missing = "01hzzzzzzzzzzzzzzzzzzzzz01";
  f.api.pages.push_back({{makeDoc(missing, Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  ASSERT_TRUE(f.engine.queueLocationChange(missing, Location::Archive, kT1));
  f.api.failPushAt = 1;
  f.api.pushStatus = ApiStatus::NotFound;
  f.api.detail = "HTTP 404";
  f.api.pages.push_back({{}, "", ApiStatus::NetworkError});

  NotFoundAsk ask;
  ask.id = missing;
  ask.drop = true;
  ask.expectTitle = true;
  f.engine.setNotFoundHooks(ReadwiseSyncEngine::NotFoundHooks{&ask, answerNotFound});

  const SyncOutcome outcome = f.engine.sync();
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.failedStage, SyncStage::Pulling);
  EXPECT_EQ(ask.calls, 1);

  ReadwiseJournal reloaded(f.store, f.engine.journalPath());
  ASSERT_TRUE(reloaded.load());
  EXPECT_TRUE(reloaded.entries().empty()) << "the dropped 404 must not be retried next sync";
  Document gone;
  EXPECT_FALSE(f.engine.findDocument(missing, gone));
}

// --- conflicts ------------------------------------------------------------

// A queued local action wins for its own field even when the document changed
// remotely in the meantime, because it is a deliberate user action not yet
// pushed. Every other field takes the remote value.
TEST(ReadwiseSync, QueuedLocalValueWinsForItsFieldOnly) {
  Fixture f;
  // The user moved the document to Shortlist locally. The server still reports
  // Later, with a newer revision than the one recorded when the op was queued --
  // so the document also changed remotely.
  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::Shortlist, kT1));
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT3, kT3)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Shortlist, 0, 10, page));
  ASSERT_EQ(page.size(), 1u) << "the local location must win for its own field";
  EXPECT_STREQ(page[0].id, "doc1");
  // Everything the user did not touch comes from the server.
  EXPECT_STREQ(page[0].updatedAt, kT3) << "non-conflicting fields must take the remote value";

  EXPECT_FALSE(f.engine.readIndexPage(Location::Later, 0, 10, page) && !page.empty())
      << "the document must not also remain under the remote location";
}

// Archiving locally takes the document out of the synced library entirely: there
// is no archive index, because archive is not a synced location.
TEST(ReadwiseSync, LocallyArchivedDocumentLeavesTheSyncedIndexes) {
  Fixture f;
  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::Archive, kT1));
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  std::vector<Document> page;
  EXPECT_FALSE(f.engine.readIndexPage(Location::Later, 0, 10, page) && !page.empty());
  EXPECT_FALSE(f.engine.readIndexPage(Location::Shortlist, 0, 10, page) && !page.empty());
  EXPECT_FALSE(f.engine.readIndexPage(Location::Feed, 0, 10, page) && !page.empty());
}

// Archiving the last Later/Shortlist document refills those shelves from the
// server. The new batch can be older than the checkpoint; Feed keeps the window.
TEST(ReadwiseSync, EmptyShelfRefillsLaterWithoutRewindingFeed) {
  Fixture f;
  constexpr const char* doneId = "01hzzzzzzzzzzzzzzzzzzzzz01";
  constexpr const char* nextId = "01hzzzzzzzzzzzzzzzzzzzzz02";
  constexpr const char* older = "2026-07-01T00:00:00.000000+00:00";

  f.api.pages.push_back({{makeDoc(doneId, Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  const size_t queriesAfterFirst = f.api.queries.size();

  ASSERT_TRUE(f.engine.queueLocationChange(doneId, Location::Archive, kT2));
  f.api.pages.push_back({{makeDoc(nextId, Location::Later, older, older)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_GT(f.api.queries.size(), queriesAfterFirst);
  const ListQuery& later = f.api.queries[queriesAfterFirst];
  EXPECT_EQ(later.location, Location::Later);
  EXPECT_STREQ(later.updatedAfter, "");

  bool sawFeed = false;
  for (size_t i = queriesAfterFirst; i < f.api.queries.size(); ++i) {
    if (f.api.queries[i].location != Location::Feed) {
      continue;
    }
    sawFeed = true;
    EXPECT_STREQ(f.api.queries[i].updatedAfter, kT2);
  }
  EXPECT_TRUE(sawFeed);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 10, page));
  ASSERT_EQ(page.size(), 1u);
  EXPECT_STREQ(page[0].id, nextId);

  Checkpoint checkpoint;
  ASSERT_TRUE(f.engine.loadCheckpoint(checkpoint));
  EXPECT_STREQ(checkpoint.updatedAfter, kT2) << "a refill must not move the checkpoint backwards";
}

// A shelf that still has a document stays on the incremental window.
TEST(ReadwiseSync, PartialShelfDoesNotRefill) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  const size_t queriesAfterFirst = f.api.queries.size();

  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::Archive, kT1));
  f.api.pages.push_back({{makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_GT(f.api.queries.size(), queriesAfterFirst);
  const ListQuery& later = f.api.queries[queriesAfterFirst];
  EXPECT_EQ(later.location, Location::Later);
  EXPECT_STREQ(later.updatedAfter, kT2);
}

// Deleting the last Later document frees the shelf, so the same sync refills it.
TEST(ReadwiseSync, DeletingTheLastDocumentRefills) {
  Fixture f;
  constexpr const char* doneId = "01hzzzzzzzzzzzzzzzzzzzzz11";
  constexpr const char* nextId = "01hzzzzzzzzzzzzzzzzzzzzz12";

  f.api.pages.push_back({{makeDoc(doneId, Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  const size_t queriesAfterFirst = f.api.queries.size();

  ASSERT_TRUE(f.engine.queueDelete(doneId, kT1));
  f.api.pages.push_back({{makeDoc(nextId, Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_GT(f.api.queries.size(), queriesAfterFirst);
  EXPECT_EQ(f.api.queries[queriesAfterFirst].location, Location::Later);
  EXPECT_STREQ(f.api.queries[queriesAfterFirst].updatedAfter, "");

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 10, page));
  ASSERT_EQ(page.size(), 1u);
  EXPECT_STREQ(page[0].id, nextId);
  Document gone;
  EXPECT_FALSE(f.engine.findDocument(doneId, gone));
}

// A body is only useful while the document is readable from the library, so
// moving to an unsynced location (archive, new) must reclaim its SD space.
TEST(ReadwiseSync, BodyIsEvictedWhenDocumentLeavesTheLibrary) {
  Fixture f;
  Document withBody = makeDoc("doc1", Location::Later, kT1, kT1);
  withBody.flags |= FLAG_HAS_BODY;
  f.api.pages.push_back({{withBody}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  const std::string body = f.engine.bodyPath("doc1");
  f.store.put(body, {'t', 'e', 'x', 't'});
  ASSERT_TRUE(f.store.has(body));

  // Now the server reports it archived.
  f.api.pages.push_back({{makeDoc("doc1", Location::Archive, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  EXPECT_FALSE(f.store.has(body)) << "an archived document's cached body must be reclaimed";
}

TEST(ReadwiseSync, QueuedSeenSurvivesMerge) {
  Fixture f;
  ASSERT_TRUE(f.engine.queueSeen("doc1", kT1));
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 10, page));
  ASSERT_EQ(page.size(), 1u);
  EXPECT_TRUE(page[0].flags & FLAG_SEEN);
}

// --- reconciliation -------------------------------------------------------

// The API emits no tombstones, so deletion is inferred from absence -- but only
// from a sweep that completed. A failed sweep must expire nothing.
TEST(ReadwiseSync, IncompleteSweepExpiresNothing) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  f.api.pages.push_back({{}, "", ApiStatus::NetworkError});
  const SyncOutcome outcome = f.engine.reconcile();
  EXPECT_FALSE(outcome.ok);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 10, page));
  EXPECT_EQ(page.size(), 2u) << "a failed sweep must never be treated as evidence of deletion";
}

// Queued actions must be visible immediately, not at the next sync: archiving
// offline removes the document from the synced indexes via rebuildLocal.
TEST(ReadwiseSync, RebuildLocalReflectsQueuedActionsOffline) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::Archive, kT1));
  ASSERT_TRUE(f.engine.rebuildLocal());

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 10, page));
  ASSERT_EQ(page.size(), 1u) << "the archived document must leave the list before any sync";
  EXPECT_STREQ(page[0].id, "doc2");
  EXPECT_EQ(f.engine.indexCount(Location::Later), 1u);

  // The op is still queued for the next sync.
  ReadwiseJournal reloaded(f.store, f.engine.journalPath());
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.entries().size(), 1u);
}

// A queued delete leaves the list immediately, same as a queued archive. The
// record stays until the server accepts, because an incremental pull cannot
// see the deletion and a failed push must still have a local copy to retry.
TEST(ReadwiseSync, RebuildLocalHidesQueuedDelete) {
  Fixture f;
  constexpr const char* gone = "01hzzzzzzzzzzzzzzzzzzzzz01";
  constexpr const char* kept = "01hzzzzzzzzzzzzzzzzzzzzz02";
  f.api.pages.push_back(
      {{makeDoc(gone, Location::Later, kT1, kT1), makeDoc(kept, Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_TRUE(f.engine.queueDelete(gone, kT1));
  ASSERT_TRUE(f.engine.rebuildLocal());

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 10, page));
  ASSERT_EQ(page.size(), 1u) << "the deleted document must leave the list before any sync";
  EXPECT_STREQ(page[0].id, kept);
  EXPECT_EQ(f.engine.indexCount(Location::Later), 1u);

  Document stillThere;
  EXPECT_TRUE(f.engine.findDocument(gone, stillThere));

  ReadwiseJournal reloaded(f.store, f.engine.journalPath());
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.entries().size(), 1u);
  EXPECT_EQ(reloaded.entries()[0].op, OpType::Delete);
}

// Ordinary navigation must not rewrite the cache: an empty journal makes
// rebuildLocal a no-op with zero durable writes.
TEST(ReadwiseSync, RebuildLocalWithEmptyJournalWritesNothing) {
  Fixture f;
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  const int writesBefore = f.store.writeCount();
  ASSERT_TRUE(f.engine.rebuildLocal());
  EXPECT_EQ(f.store.writeCount(), writesBefore) << "a no-op rebuild must not touch the SD card";
}

// Without persisting FLAG_HAS_BODY, a downloaded article would show as "not
// downloaded" after the post-download restart and be fetched again.
TEST(ReadwiseSync, SetBodyCachedPersistsAcrossReload) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_TRUE(f.engine.setBodyCached("doc1", true));

  // A fresh engine over the same store simulates the restart.
  ReadwiseSyncEngine reloaded(f.api, f.store, kBase);
  Document doc;
  ASSERT_TRUE(reloaded.findDocument("doc1", doc));
  EXPECT_TRUE(doc.flags & FLAG_HAS_BODY);
  ASSERT_TRUE(reloaded.findDocument("doc2", doc));
  EXPECT_FALSE(doc.flags & FLAG_HAS_BODY) << "only the patched record may change";

  // And the index-read path sees it too.
  std::vector<Document> page;
  ASSERT_TRUE(reloaded.readIndexPage(Location::Later, 0, 10, page));
  bool found = false;
  for (const Document& d : page) {
    if (std::string(d.id) == "doc1") {
      found = true;
      EXPECT_TRUE(d.flags & FLAG_HAS_BODY);
    }
  }
  EXPECT_TRUE(found);

  EXPECT_FALSE(f.engine.setBodyCached("missing", true));
}

namespace {
struct BodyHookRecorder {
  std::vector<std::pair<uint16_t, uint16_t>> progress;
  std::vector<uint32_t> sleeps;
  readwise::ReadwiseSyncEngine::BodySyncHooks hooks() {
    readwise::ReadwiseSyncEngine::BodySyncHooks h;
    h.ctx = this;
    h.onProgress = [](void* ctx, uint16_t done, uint16_t total) {
      static_cast<BodyHookRecorder*>(ctx)->progress.push_back({done, total});
    };
    h.sleepMs = [](void* ctx, uint32_t ms) { static_cast<BodyHookRecorder*>(ctx)->sleeps.push_back(ms); };
    return h;
  }
};
}  // namespace

// Sync-time prefetch: every cached document without a body gets one, so after
// a sync the entire library reads offline.
TEST(ReadwiseSync, DownloadMissingBodiesFetchesOnlyMissing) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  f.api.bodies["doc1"] = "<p>uno</p>";
  f.api.bodies["doc2"] = "<p>dos</p>";

  // doc2 already has a cached body on disk and the flag set.
  ASSERT_TRUE(f.engine.setBodyCached("doc2", true));
  const std::string doc2Path = f.engine.bodyPath("doc2");
  f.store.put(doc2Path, {'x'});

  BodyHookRecorder rec;
  const auto outcome = f.engine.downloadMissingBodies(rec.hooks());
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.total, 1);
  EXPECT_EQ(outcome.downloaded, 1);
  EXPECT_EQ(outcome.failed, 0);
  EXPECT_EQ(f.api.bodyFetches, 1) << "the cached document must not be re-fetched";
  EXPECT_TRUE(f.store.has(f.engine.bodyPath("doc1")));
  // An upfront (0, total) call publishes the denominator before the first
  // fetch, so the caller's popup opens at 0/N rather than 0/0.
  ASSERT_EQ(rec.progress.size(), 2u);
  EXPECT_EQ(rec.progress[0], (std::pair<uint16_t, uint16_t>{0, 1}));
  EXPECT_EQ(rec.progress[1], (std::pair<uint16_t, uint16_t>{1, 1}));

  Document doc;
  ASSERT_TRUE(f.engine.findDocument("doc1", doc));
  EXPECT_TRUE(doc.flags & FLAG_HAS_BODY);

  // A second pass has nothing to do.
  const auto again = f.engine.downloadMissingBodies(rec.hooks());
  EXPECT_TRUE(again.ok);
  EXPECT_EQ(again.total, 0);
}

// Throttling is expected -- body fetches share the list endpoint's budget. The
// engine waits out retry-after via the sleep hook and retries that document.
TEST(ReadwiseSync, DownloadMissingBodiesWaitsOutRateLimit) {
  Fixture f;
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  f.api.bodies["doc1"] = "<p>uno</p>";
  f.api.bodyRateLimitFirstN = 1;

  BodyHookRecorder rec;
  const auto outcome = f.engine.downloadMissingBodies(rec.hooks());
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.downloaded, 1);
  ASSERT_EQ(rec.sleeps.size(), 1u);
  EXPECT_EQ(rec.sleeps[0], 16000u) << "the server's retry-after drives the wait";
}

// A single bad document must not sink the pass; it is skipped and the
// on-demand path remains its retry.
TEST(ReadwiseSync, DownloadMissingBodiesSkipsFailuresButAbortsOnAuth) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  // doc1 has no body entry in the fake -> ServerError; doc2 succeeds.
  f.api.bodies["doc2"] = "<p>dos</p>";

  BodyHookRecorder rec;
  const auto outcome = f.engine.downloadMissingBodies(rec.hooks());
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.total, 2);
  EXPECT_EQ(outcome.downloaded, 1);
  EXPECT_EQ(outcome.failed, 1);

  Document doc;
  ASSERT_TRUE(f.engine.findDocument("doc1", doc));
  EXPECT_FALSE(doc.flags & FLAG_HAS_BODY) << "a failed fetch must not claim a cached body";
}

TEST(ReadwiseSync, FindDocumentById) {
  Fixture f;
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Shortlist, kT2, kT2)},
                         "",
                         ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  Document found;
  ASSERT_TRUE(f.engine.findDocument("doc2", found));
  EXPECT_EQ(found.location, Location::Shortlist);
  EXPECT_FALSE(f.engine.findDocument("missing", found));
  EXPECT_FALSE(f.engine.findDocument(nullptr, found));
}

// --- feed -----------------------------------------------------------------
//
// The pull walks the synced locations in policy-table order (Later, Shortlist,
// Feed) and the fake serves pages FIFO regardless of location, so a feed page
// must be preceded by one page for each non-feed location.

namespace {
void pushEmptyPagesForNonFeed(Fixture& f) {
  f.api.pages.push_back({{}, "", ApiStatus::Ok});  // Later
  f.api.pages.push_back({{}, "", ApiStatus::Ok});  // Shortlist
}

Document makeSeenDoc(const char* id, Location location, const char* updatedAt, const char* lastMovedAt) {
  Document doc = makeDoc(id, location, updatedAt, lastMovedAt);
  doc.flags |= FLAG_SEEN;
  return doc;
}
}  // namespace

// The API has no server-side unread filter, so read feed items are skipped
// client-side -- but their updated_at still advances the checkpoint, or the
// same read items would be re-walked on every sync.
TEST(ReadwiseSync, FeedPullSkipsSeenDocuments) {
  Fixture f;
  pushEmptyPagesForNonFeed(f);
  f.api.pages.push_back(
      {{makeDoc("f1", Location::Feed, kT1, kT1), makeSeenDoc("f2", Location::Feed, kT2, kT2)}, "", ApiStatus::Ok});

  const SyncOutcome outcome = f.engine.sync();
  ASSERT_TRUE(outcome.ok) << "failed at stage " << static_cast<int>(outcome.failedStage);
  EXPECT_EQ(outcome.pulled, 1);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Feed, 0, 10, page));
  ASSERT_EQ(page.size(), 1u);
  EXPECT_STREQ(page[0].id, "f1");

  Checkpoint checkpoint;
  ASSERT_TRUE(f.engine.loadCheckpoint(checkpoint));
  EXPECT_STREQ(checkpoint.updatedAfter, kT2) << "a skipped read item must still advance the cursor";
}

// Feed is a firehose; once the cap is collected the walk must stop rather than
// paginate to a null cursor.
TEST(ReadwiseSync, FeedPullStopsAtCapWithoutWalkingMorePages) {
  Fixture f;
  f.engine.setFeedCap(2);
  pushEmptyPagesForNonFeed(f);
  f.api.pages.push_back(
      {{makeDoc("f1", Location::Feed, kT2, kT2), makeDoc("f2", Location::Feed, kT1, kT1)}, "cursor1", ApiStatus::Ok});
  f.api.pages.push_back({{makeDoc("f3", Location::Feed, kT3, kT3)}, "", ApiStatus::Ok});

  ASSERT_TRUE(f.engine.sync().ok);
  EXPECT_EQ(f.engine.indexCount(Location::Feed), 2u);
  EXPECT_EQ(f.api.pageIndex, 3u) << "the page past the cap must not be fetched";
  EXPECT_EQ(f.api.queries.size(), 3u);
}

// Whether the API caps pagination at 10,000 documents is untested territory;
// the feed walk must never get anywhere near it.
TEST(ReadwiseSync, FeedPaginationIsBoundedByMaxPages) {
  Fixture f;
  pushEmptyPagesForNonFeed(f);
  // Every feed page returns a cursor, simulating an endless firehose of read
  // items (nothing staged, so the cap never trips).
  for (int i = 0; i < FEED_MAX_PAGES + 2; ++i) {
    const std::string id = "f" + std::to_string(i);
    f.api.pages.push_back({{makeSeenDoc(id.c_str(), Location::Feed, kT1, kT1)}, "more", ApiStatus::Ok});
  }

  ASSERT_TRUE(f.engine.sync().ok);
  EXPECT_EQ(f.api.queries.size(), 2u + FEED_MAX_PAGES) << "the feed walk must stop at FEED_MAX_PAGES";
}

// "Never sync read feed items" has a cleanup side: an item read on the device
// is removed -- record, index entry and body -- at the next sync.
TEST(ReadwiseSync, SeenFeedDocIsRemovedOnNextSync) {
  Fixture f;
  pushEmptyPagesForNonFeed(f);
  f.api.pages.push_back({{makeDoc("f1", Location::Feed, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  const std::string body = f.engine.bodyPath("f1");
  f.store.put(body, {'t', 'e', 'x', 't'});
  ASSERT_TRUE(f.engine.setBodyCached("f1", true));
  ASSERT_TRUE(f.engine.queueSeen("f1", kT1));

  // Next sync pulls nothing new; the queued seen expires the item at merge.
  ASSERT_TRUE(f.engine.sync().ok);

  Document doc;
  EXPECT_FALSE(f.engine.findDocument("f1", doc)) << "a read feed item must leave docs.bin at the next sync";
  EXPECT_EQ(f.engine.indexCount(Location::Feed), 0u);
  EXPECT_FALSE(f.store.has(body)) << "the body goes with the record";
}

// The same cleanup for an item read on ANOTHER device: the next pull returns
// it seen, the sink deliberately stages no replacement -- and the stale unseen
// record in docs.bin must die with it rather than being carried over forever.
TEST(ReadwiseSync, RemotelySeenFeedDocIsRemovedOnNextSync) {
  Fixture f;
  pushEmptyPagesForNonFeed(f);
  f.api.pages.push_back({{makeDoc("f1", Location::Feed, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  const std::string body = f.engine.bodyPath("f1");
  f.store.put(body, {'t', 'e', 'x', 't'});
  ASSERT_TRUE(f.engine.setBodyCached("f1", true));

  // Read elsewhere: the server now reports it seen with a later updated_at.
  pushEmptyPagesForNonFeed(f);
  f.api.pages.push_back({{makeSeenDoc("f1", Location::Feed, kT2, kT2)}, "", ApiStatus::Ok});
  const SyncOutcome outcome = f.engine.sync();
  ASSERT_TRUE(outcome.ok) << "failed at stage " << static_cast<int>(outcome.failedStage);
  EXPECT_EQ(outcome.pulled, 0) << "a seen replacement is an eviction, not a pulled document";

  Document doc;
  EXPECT_FALSE(f.engine.findDocument("f1", doc)) << "a feed item read elsewhere must leave docs.bin at the next sync";
  EXPECT_EQ(f.engine.indexCount(Location::Feed), 0u);
  EXPECT_FALSE(f.store.has(body)) << "the body goes with the record";
}

// The removal is deliberately deferred to the next sync: rebuildLocal runs on
// every library entry, and the article the user just read must stay openable.
TEST(ReadwiseSync, RebuildLocalDoesNotRemoveSeenFeedDoc) {
  Fixture f;
  pushEmptyPagesForNonFeed(f);
  f.api.pages.push_back({{makeDoc("f1", Location::Feed, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_TRUE(f.engine.queueSeen("f1", kT1));
  ASSERT_TRUE(f.engine.rebuildLocal());

  Document doc;
  EXPECT_TRUE(f.engine.findDocument("f1", doc)) << "reading a feed item must not expire it before the next sync";
  EXPECT_EQ(f.engine.indexCount(Location::Feed), 1u);
}

// Feed cannot be swept for deletions -- it exceeds the API's count cap on its
// own -- so a completed sweep that never saw a feed doc must not expire it.
TEST(ReadwiseSync, ReconcileSweepExcludesFeed) {
  Fixture f;
  f.api.pages.push_back({{makeDoc("L1", Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  f.api.pages.push_back({{}, "", ApiStatus::Ok});  // Shortlist
  f.api.pages.push_back({{makeDoc("f1", Location::Feed, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  const std::string body = f.engine.bodyPath("f1");
  f.store.put(body, {'t', 'e', 'x', 't'});
  ASSERT_TRUE(f.engine.setBodyCached("f1", true));

  // The sweep answers for Later and Shortlist only.
  const size_t queriesBefore = f.api.queries.size();
  f.api.pages.push_back({{makeDoc("L1", Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  f.api.pages.push_back({{}, "", ApiStatus::Ok});  // Shortlist
  ASSERT_TRUE(f.engine.reconcile().ok);

  for (size_t i = queriesBefore; i < f.api.queries.size(); ++i) {
    EXPECT_NE(f.api.queries[i].location, Location::Feed) << "the sweep must never enumerate feed";
  }
  Document doc;
  EXPECT_TRUE(f.engine.findDocument("f1", doc)) << "absence from a sweep that excludes feed proves nothing";
  EXPECT_TRUE(f.store.has(body));
}

// When the cap displaces feed items, the newest survive: staged documents are
// written before carry-over, and docs.bin holds newest-first within a pull.
TEST(ReadwiseSync, FeedCapKeepsNewestAtMerge) {
  Fixture f;
  f.engine.setFeedCap(2);
  pushEmptyPagesForNonFeed(f);
  f.api.pages.push_back(
      {{makeDoc("f2", Location::Feed, kT2, kT2), makeDoc("f1", Location::Feed, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  const std::string oldBody = f.engine.bodyPath("f1");
  f.store.put(oldBody, {'t', 'e', 'x', 't'});
  ASSERT_TRUE(f.engine.setBodyCached("f1", true));

  // A newer item arrives; the cap is 2, so the oldest carried-over item goes.
  pushEmptyPagesForNonFeed(f);
  f.api.pages.push_back({{makeDoc("f3", Location::Feed, kT3, kT3)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Feed, 0, 10, page));
  ASSERT_EQ(page.size(), 2u);
  EXPECT_STREQ(page[0].id, "f3");
  EXPECT_STREQ(page[1].id, "f2");
  Document doc;
  EXPECT_FALSE(f.engine.findDocument("f1", doc)) << "the displaced item must leave docs.bin";
  EXPECT_FALSE(f.store.has(oldBody)) << "a displaced feed item's body is reclaimed";
}

// Bodies are prefetched only for documents readable from the library: feed is
// included, unsynced leftovers (archive) are not.
TEST(ReadwiseSync, BodyPrefetchSkipsUnsyncedLocationsAndIncludesFeed) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("L1", Location::Later, kT1, kT1), makeDoc("A1", Location::Archive, kT2, kT2)}, "", ApiStatus::Ok});
  f.api.pages.push_back({{}, "", ApiStatus::Ok});  // Shortlist
  f.api.pages.push_back({{makeDoc("f1", Location::Feed, kT3, kT3)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  f.api.bodies["L1"] = "<p>later</p>";
  f.api.bodies["A1"] = "<p>archived</p>";
  f.api.bodies["f1"] = "<p>feed</p>";

  BodyHookRecorder rec;
  const auto outcome = f.engine.downloadMissingBodies(rec.hooks());
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.total, 2);
  EXPECT_EQ(outcome.downloaded, 2);
  EXPECT_EQ(f.api.bodyFetches, 2) << "an archived leftover must not cost a fetch";
  EXPECT_TRUE(f.store.has(f.engine.bodyPath("L1")));
  EXPECT_TRUE(f.store.has(f.engine.bodyPath("f1")));
  EXPECT_FALSE(f.store.has(f.engine.bodyPath("A1")));
}

// Upgrade path: the Inbox ("new") view no longer exists. Its index file, any
// new-location records, and their bodies are all cleared by the first sync.
TEST(ReadwiseSync, UpgradeDropsNewLocationArtifacts) {
  Fixture f;
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  const std::string body = f.engine.bodyPath("doc1");
  f.store.put(body, {'t', 'e', 'x', 't'});
  ASSERT_TRUE(f.engine.setBodyCached("doc1", true));
  // A pre-upgrade sync left an inbox index behind.
  f.store.put(f.engine.indexPath(Location::New), {0x01, 0x00});

  // The server has since moved the document to the inbox.
  f.api.pages.push_back({{makeDoc("doc1", Location::New, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  EXPECT_FALSE(f.store.has(f.engine.indexPath(Location::New))) << "the orphaned inbox index must be removed";
  Document doc;
  EXPECT_FALSE(f.engine.findDocument("doc1", doc)) << "inbox documents are no longer cached";
  EXPECT_FALSE(f.store.has(body));
}

TEST(ReadwiseSync, CompletedSweepExpiresMissingDocuments) {
  Fixture f;
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  // The sweep reports only doc1, so doc2 was deleted remotely.
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  const SyncOutcome outcome = f.engine.reconcile();
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.retained, 1);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 10, page));
  ASSERT_EQ(page.size(), 1u);
  EXPECT_STREQ(page[0].id, "doc1");
}

TEST(ReadwiseSync, LibraryCountsGroupCategoryUnreadAndOnDevice) {
  Fixture f;
  auto unread = makeDoc("a1", Location::Later, kT1, kT1);
  auto seen = makeDoc("a2", Location::Later, kT2, kT2);
  seen.flags = FLAG_SEEN;
  auto email = makeDoc("e1", Location::Later, kT3, kT3);
  email.category = Category::Email;
  f.api.pages.push_back({{unread, seen, email}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  ASSERT_TRUE(f.engine.setBodyCached("a1", true));
  ASSERT_TRUE(f.engine.queueSeen("e1", kT3));

  // A fresh engine has to reload the journal; the queued seen must still count.
  ReadwiseSyncEngine again(f.api, f.store, kBase);
  LibraryCounts counts;
  ASSERT_TRUE(again.collectLibraryCounts(counts));

  const auto article = static_cast<int>(Category::Article);
  const auto emailSlot = static_cast<int>(Category::Email);
  EXPECT_EQ(counts.named[article].total, 2);
  EXPECT_EQ(counts.named[article].unread, 1);
  EXPECT_EQ(counts.named[article].onDevice, 1);
  EXPECT_EQ(counts.named[emailSlot].total, 1);
  EXPECT_EQ(counts.named[emailSlot].unread, 0);
  EXPECT_EQ(counts.named[emailSlot].onDevice, 0);
  EXPECT_EQ(counts.named[static_cast<int>(Category::Rss)].total, 0);
  EXPECT_EQ(counts.other.total, 0);
}

TEST(ReadwiseSync, LibraryCountsMissingFile) {
  Fixture f;
  LibraryCounts counts;
  EXPECT_FALSE(f.engine.collectLibraryCounts(counts));
}

TEST(ReadwiseSync, LengthSlotsSplitLaterAndShortlistByWordCount) {
  Fixture f;
  std::vector<uint16_t> slots;
  ASSERT_TRUE(f.engine.collectLengthSlots(1500, true, nullptr, slots));
  EXPECT_TRUE(slots.empty());

  auto withWords = [](const char* id, Location location, const char* when, uint32_t words, const char* author) {
    Document doc = makeDoc(id, location, when, when);
    doc.wordCount = words;
    copyBounded(doc.author, AUTHOR_CAP, author, strlen(author));
    return doc;
  };
  f.api.pages.push_back({{
                             withWords("later-long", Location::Later, kT1, 2000, "Ada"),
                             withWords("later-quick", Location::Later, kT3, 400, "Ada"),
                             withWords("later-zero", Location::Later, kT2, 0, "Ada"),
                             withWords("short-edge", Location::Shortlist, kT2, 1500, "Bea"),
                             withWords("short-quick", Location::Shortlist, kT1, 1499, "Bea"),
                             withWords("feed-long", Location::Feed, kT3, 5000, "Ada"),
                         },
                         "",
                         ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_TRUE(f.engine.collectLengthSlots(1500, true, nullptr, slots));
  std::vector<Document> docs;
  ASSERT_TRUE(f.engine.readRecords(slots.data(), static_cast<uint16_t>(slots.size()), docs));
  ASSERT_EQ(docs.size(), 2u);
  EXPECT_STREQ(docs[0].id, "short-edge");
  EXPECT_STREQ(docs[1].id, "later-long");

  ASSERT_TRUE(f.engine.collectLengthSlots(1500, false, nullptr, slots));
  ASSERT_TRUE(f.engine.readRecords(slots.data(), static_cast<uint16_t>(slots.size()), docs));
  ASSERT_EQ(docs.size(), 2u);
  EXPECT_STREQ(docs[0].id, "later-quick");
  EXPECT_STREQ(docs[1].id, "short-quick");

  ASSERT_TRUE(f.engine.collectLengthSlots(1500, true, "Ada", slots));
  ASSERT_TRUE(f.engine.readRecords(slots.data(), static_cast<uint16_t>(slots.size()), docs));
  ASSERT_EQ(docs.size(), 1u);
  EXPECT_STREQ(docs[0].id, "later-long");

  EXPECT_TRUE(f.engine.readRecords(nullptr, 0, docs));
  EXPECT_TRUE(docs.empty());
}

TEST(ReadwiseSync, HighlightIsPostedOnceOnSync) {
  Fixture f;
  constexpr const char* id = "01hzzzzzzzzzzzzzzzzzzzzz08";
  f.api.pages.push_back({{makeDoc(id, Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  ASSERT_TRUE(f.engine.appendHighlight(id, "Call me Ishmael"));
  ASSERT_TRUE(f.engine.appendHighlight(id, "Call me Ishmael"));
  const SyncOutcome posted = f.engine.sync();
  ASSERT_TRUE(posted.ok) << "failed at stage " << static_cast<int>(posted.failedStage);
  ASSERT_EQ(f.api.highlights.size(), 1u);
  EXPECT_EQ(posted.highlightsSent, 1);
  EXPECT_EQ(posted.highlightsFailed, 0);
  EXPECT_EQ(f.api.highlights[0].text, "Call me Ishmael");
  EXPECT_EQ(f.api.highlights[0].title, "Title");
  EXPECT_TRUE(f.store.has(std::string(kBase) + "/highlights/" + id + ".bin"));
  EXPECT_FALSE(f.store.has(std::string(kBase) + "/bodies/" + id + "/highlights.bin"));

  f.api.highlights.clear();
  ASSERT_TRUE(f.engine.sync().ok);
  EXPECT_TRUE(f.api.highlights.empty());
}

TEST(ReadwiseSync, HighlightPushFailureStaysQueued) {
  Fixture f;
  constexpr const char* id = "01hzzzzzzzzzzzzzzzzzzzzz08";
  ASSERT_TRUE(f.engine.appendHighlight(id, "A quote with \"marks\""));
  f.api.failHighlights = true;
  const SyncOutcome failed = f.engine.sync();
  EXPECT_FALSE(failed.ok);
  EXPECT_EQ(failed.failedStage, SyncStage::Pushing);
  EXPECT_NE(std::string(failed.detail).find("highlight"), std::string::npos);
  EXPECT_NE(std::string(failed.detail).find(id), std::string::npos);
  EXPECT_TRUE(f.api.highlights.empty());
  EXPECT_EQ(failed.highlightsSent, 0);
  EXPECT_EQ(failed.highlightsFailed, 0);

  f.api.failHighlights = false;
  ASSERT_TRUE(f.engine.sync().ok);
  ASSERT_EQ(f.api.highlights.size(), 1u);
  EXPECT_EQ(f.api.highlights[0].text, "A quote with \"marks\"");
  EXPECT_TRUE(f.api.highlights[0].title.empty());
}

TEST(ReadwiseSync, ArchiveMovesHighlightOutOfTheArticleDir) {
  Fixture f;
  constexpr const char* id = "01hzzzzzzzzzzzzzzzzzzzzz08";
  Document doc = makeDoc(id, Location::Later, kT1, kT1);
  doc.flags |= FLAG_HAS_BODY;
  f.api.pages.push_back({{doc}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  const std::string body = f.engine.bodyPath(id);
  f.store.put(body, {'e', 'p', 'u', 'b'});
  ASSERT_TRUE(f.engine.appendHighlight(id, "A sentence worth keeping"));

  const std::string kept = std::string(kBase) + "/highlights/" + id + ".bin";
  const std::string legacy = std::string(kBase) + "/bodies/" + id + "/highlights.bin";
  const std::vector<uint8_t> bytes = f.store.files().at(kept);
  f.store.put(legacy, bytes);
  ASSERT_TRUE(f.store.remove(kept));

  ASSERT_TRUE(f.engine.queueLocationChange(id, Location::Archive, kT1));
  ASSERT_TRUE(f.engine.rebuildLocal());
  EXPECT_FALSE(f.store.has(body)) << "the cached article is still evicted";
  EXPECT_FALSE(f.store.has(legacy));
  EXPECT_TRUE(f.store.has(kept));

  const SyncOutcome posted = f.engine.sync();
  ASSERT_TRUE(posted.ok) << posted.detail;
  EXPECT_EQ(posted.highlightsSent, 1);
  ASSERT_EQ(f.api.highlights.size(), 1u);
  EXPECT_EQ(f.api.highlights[0].text, "A sentence worth keeping");
  EXPECT_EQ(f.api.highlights[0].title, "Title");
  EXPECT_TRUE(f.store.has(kept));
}

TEST(ReadwiseSync, ForgetDocumentKeepsUnsentHighlight) {
  Fixture f;
  constexpr const char* id = "01hzzzzzzzzzzzzzzzzzzzzz08";
  f.api.pages.push_back({{makeDoc(id, Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  ASSERT_TRUE(f.engine.appendHighlight(id, "Still queued"));

  const std::string kept = std::string(kBase) + "/highlights/" + id + ".bin";
  ASSERT_TRUE(f.engine.forgetDocument(id));
  EXPECT_TRUE(f.store.has(kept));
  Document gone;
  EXPECT_FALSE(f.engine.findDocument(id, gone));

  const SyncOutcome posted = f.engine.sync();
  ASSERT_TRUE(posted.ok) << posted.detail;
  EXPECT_EQ(posted.highlightsSent, 1);
  ASSERT_EQ(f.api.highlights.size(), 1u);
  EXPECT_EQ(f.api.highlights[0].text, "Still queued");
  EXPECT_EQ(f.api.highlights[0].title, "Title");
}

TEST(ReadwiseSync, LegacyHighlightIsMigratedWhenPushed) {
  Fixture f;
  constexpr const char* id = "01hzzzzzzzzzzzzzzzzzzzzz08";
  ASSERT_TRUE(f.engine.appendHighlight(id, "From an older build"));
  const std::string kept = std::string(kBase) + "/highlights/" + id + ".bin";
  const std::string legacy = std::string(kBase) + "/bodies/" + id + "/highlights.bin";
  const std::vector<uint8_t> bytes = f.store.files().at(kept);
  f.store.put(legacy, bytes);
  ASSERT_TRUE(f.store.remove(kept));

  const SyncOutcome posted = f.engine.sync();
  ASSERT_TRUE(posted.ok) << posted.detail;
  EXPECT_EQ(posted.highlightsSent, 1);
  EXPECT_TRUE(f.store.has(kept));
  EXPECT_FALSE(f.store.has(legacy));
  ASSERT_EQ(f.api.highlights.size(), 1u);
  EXPECT_EQ(f.api.highlights[0].text, "From an older build");
}

TEST(ReadwiseSync, RejectedHighlightIsSkippedAndDoesNotBlockSync) {
  Fixture f;
  constexpr const char* id = "01hzzzzzzzzzzzzzzzzzzzzz08";
  f.api.pages.push_back({{makeDoc(id, Location::Later, kT1, kT1)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  ASSERT_TRUE(f.engine.appendHighlight(id, "Bad quote"));
  ASSERT_TRUE(f.engine.appendHighlight(id, "Good quote"));
  ASSERT_TRUE(f.engine.queueSeen(id, kT1));
  f.api.rejectHighlightCount = 1;
  f.api.detail = "HTTP 400 {\"detail\":\"bad highlight\"}";

  const SyncOutcome outcome = f.engine.sync();
  ASSERT_TRUE(outcome.ok) << outcome.detail;
  EXPECT_EQ(outcome.highlightsSent, 1);
  EXPECT_EQ(outcome.highlightsFailed, 1);
  EXPECT_NE(std::string(outcome.highlightDetail).find("HTTP 400"), std::string::npos);
  EXPECT_NE(std::string(outcome.highlightDetail).find(id), std::string::npos);
  ASSERT_EQ(f.api.highlights.size(), 1u);
  EXPECT_EQ(f.api.highlights[0].text, "Good quote");
  ASSERT_EQ(f.api.pushed.size(), 1u);
  EXPECT_EQ(f.api.pushed[0].op, OpType::SetSeen);

  f.api.highlights.clear();
  f.api.detail = "";
  const SyncOutcome again = f.engine.sync();
  ASSERT_TRUE(again.ok);
  EXPECT_EQ(again.highlightsSent, 0);
  EXPECT_EQ(again.highlightsFailed, 0);
  EXPECT_TRUE(f.api.highlights.empty());
  EXPECT_TRUE(f.store.has(std::string(kBase) + "/highlights/" + id + ".bin"));
}

TEST(ReadwiseSync, QueuedListShowsArchiveAndDeleteAndUndo) {
  Fixture f;
  constexpr const char* archived = "01hzzzzzzzzzzzzzzzzzzzzz21";
  constexpr const char* deleted = "01hzzzzzzzzzzzzzzzzzzzzz22";
  constexpr const char* unknown = "01hzzzzzzzzzzzzzzzzzzzzz23";
  f.api.pages.push_back(
      {{makeDoc(archived, Location::Later, kT1, kT1), makeDoc(deleted, Location::Later, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);
  ASSERT_TRUE(f.engine.queueLocationChange(archived, Location::Archive, kT1));
  ASSERT_TRUE(f.engine.queueDelete(deleted, kT1));
  ASSERT_TRUE(f.engine.queueDelete(unknown, kT1));
  ASSERT_TRUE(f.engine.queueSeen(archived, kT1));

  std::vector<ReadwiseSyncEngine::QueuedDocument> rows;
  ASSERT_TRUE(f.engine.collectQueued(rows));
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_STREQ(rows[0].id, archived);
  EXPECT_FALSE(rows[0].deleted);
  EXPECT_STREQ(rows[0].title, "Title");
  EXPECT_STREQ(rows[1].id, deleted);
  EXPECT_TRUE(rows[1].deleted);
  EXPECT_STREQ(rows[2].id, unknown);
  EXPECT_TRUE(rows[2].deleted);
  EXPECT_STREQ(rows[2].title, unknown);

  ASSERT_TRUE(f.engine.undoQueued(archived));
  ASSERT_TRUE(f.engine.rebuildLocal());
  ASSERT_TRUE(f.engine.collectQueued(rows));
  ASSERT_EQ(rows.size(), 2u);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::Later, 0, 10, page));
  bool back = false;
  for (const Document& doc : page) {
    if (strcmp(doc.id, archived) == 0) {
      back = true;
    }
  }
  EXPECT_TRUE(back);
  EXPECT_FALSE(f.engine.undoQueued("short"));
}
