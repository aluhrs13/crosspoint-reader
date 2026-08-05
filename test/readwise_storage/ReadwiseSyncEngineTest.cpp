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
  f.api.pages.push_back({{makeDoc("doc2", Location::Later, kT3, kT3)}, "", ApiStatus::Ok});
  const int writesBefore = f.store.writeCount();
  f.store.failAtWrite(writesBefore + 5);

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

// A push failure mid-queue must keep the unacknowledged operations, so nothing
// the user asked for is silently dropped.
TEST(ReadwiseSync, FailedPushPreservesUnacknowledgedOps) {
  Fixture f;
  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::Archive, kT1));
  ASSERT_TRUE(f.engine.queueLocationChange("doc2", Location::Archive, kT1));
  ASSERT_TRUE(f.engine.queueLocationChange("doc3", Location::Archive, kT1));
  f.api.failPushAt = 2;

  const SyncOutcome outcome = f.engine.sync();
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.failedStage, SyncStage::Pushing);
  EXPECT_EQ(outcome.pushed, 1);

  ReadwiseJournal reloaded(f.store, f.engine.journalPath());
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.entries().size(), 2u) << "the acknowledged op is dropped, the rest are kept";
  EXPECT_STREQ(reloaded.entries()[0].id, "doc2");
}

// --- conflicts ------------------------------------------------------------

// A queued local action wins for its own field even when the document changed
// remotely in the meantime, because it is a deliberate user action not yet
// pushed. Every other field takes the remote value.
TEST(ReadwiseSync, QueuedLocalValueWinsForItsFieldOnly) {
  Fixture f;
  // The user moved the document to New locally. The server still reports Later,
  // with a newer revision than the one recorded when the op was queued -- so the
  // document also changed remotely.
  ASSERT_TRUE(f.engine.queueLocationChange("doc1", Location::New, kT1));
  f.api.pages.push_back({{makeDoc("doc1", Location::Later, kT3, kT3)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  std::vector<Document> page;
  ASSERT_TRUE(f.engine.readIndexPage(Location::New, 0, 10, page));
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
  EXPECT_FALSE(f.engine.readIndexPage(Location::New, 0, 10, page) && !page.empty());
}

// A body is only useful while the document is readable from the library, so
// moving to archive or feed must reclaim its SD space.
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
  ASSERT_EQ(rec.progress.size(), 1u);
  EXPECT_EQ(rec.progress[0], (std::pair<uint16_t, uint16_t>{1, 1}));

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
  f.api.pages.push_back(
      {{makeDoc("doc1", Location::Later, kT1, kT1), makeDoc("doc2", Location::New, kT2, kT2)}, "", ApiStatus::Ok});
  ASSERT_TRUE(f.engine.sync().ok);

  Document found;
  ASSERT_TRUE(f.engine.findDocument("doc2", found));
  EXPECT_EQ(found.location, Location::New);
  EXPECT_FALSE(f.engine.findDocument("missing", found));
  EXPECT_FALSE(f.engine.findDocument(nullptr, found));
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
