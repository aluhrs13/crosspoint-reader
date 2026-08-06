// ReadwiseHighlightSync push tests against FakeApi + FakeFileStore.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "FakeReadwise.h"
#include "lib/Readwise/ReadwiseCodec.h"
#include "lib/Readwise/ReadwiseHighlightStore.h"
#include "lib/Readwise/ReadwiseHighlightSync.h"
#include "lib/Readwise/ReadwiseSyncEngine.h"

using namespace readwise;
using testing_support::FakeApi;
using testing_support::FakeFileStore;

namespace {

constexpr const char* BASE = "/.crosspoint/readwise";
constexpr const char* DOC_A = "01hxyzabcdefghjkmnpqrstvw";
constexpr const char* DOC_B = "01habcdefghjkmnpqrstvwxyz";

HighlightRecord makeRecord(const char* text, uint32_t start = 0) {
  HighlightRecord rec;
  rec.startByte = start;
  rec.endByte = start + static_cast<uint32_t>(strlen(text));
  rec.bodySize = 5000;
  rec.textLen = static_cast<uint16_t>(strlen(text));
  memcpy(rec.text, text, rec.textLen);
  rec.text[rec.textLen] = '\0';
  return rec;
}

// Writes a one-document docs.bin so engine.findDocument() can supply
// title/source_url for the attach fields.
void putDocsBin(FakeFileStore& fake, const char* id, const char* title, const char* sourceUrl) {
  Document doc;
  copyBounded(doc.id, ID_CAP, id, strlen(id));
  copyBounded(doc.title, TITLE_CAP, title, strlen(title));
  copyBounded(doc.sourceUrl, SOURCE_URL_CAP, sourceUrl, strlen(sourceUrl));
  doc.location = Location::Later;
  doc.category = Category::Article;

  uint8_t record[MAX_ENCODED_RECORD];
  const size_t recordLen = encodeDocument(doc, record, sizeof(record));
  ASSERT_GT(recordLen, 0u);

  DocsHeader header;
  header.lutOffset = static_cast<uint32_t>(DOCS_HEADER_SIZE + recordLen);
  header.recordCount = 1;
  uint8_t headerBuf[DOCS_HEADER_SIZE];
  ASSERT_EQ(encodeDocsHeader(header, headerBuf, sizeof(headerBuf)), DOCS_HEADER_SIZE);

  std::vector<uint8_t> file(headerBuf, headerBuf + DOCS_HEADER_SIZE);
  file.insert(file.end(), record, record + recordLen);
  const uint32_t recordOffset = DOCS_HEADER_SIZE;
  file.push_back(recordOffset & 0xFF);
  file.push_back((recordOffset >> 8) & 0xFF);
  file.push_back((recordOffset >> 16) & 0xFF);
  file.push_back((recordOffset >> 24) & 0xFF);
  fake.put(std::string(BASE) + "/docs.bin", file);
}

class HighlightSyncTest : public ::testing::Test {
 protected:
  FakeFileStore fake;
  FakeApi api;
  ReadwiseSyncEngine engine{api, fake, BASE};
  ReadwiseHighlightStore store{fake, BASE};
  ReadwiseHighlightSync sync{api, engine, store};
  HighlightSyncHooks hooks;

  std::vector<std::array<char, ID_CAP>> pendingIds() {
    std::vector<std::array<char, ID_CAP>> ids;
    store.loadPendingDocIds(ids);
    return ids;
  }
};

TEST_F(HighlightSyncTest, NoPendingIsSuccess) {
  const auto outcome = sync.push(hooks);
  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.uploaded, 0);
  EXPECT_EQ(api.createHighlightsCalls, 0);
}

TEST_F(HighlightSyncTest, HappyPathUploadsMarksAndTrimsPending) {
  putDocsBin(fake, DOC_A, "An Article", "https://example.com/a");
  ASSERT_TRUE(store.append(DOC_A, makeRecord("First sentence.")));
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Second sentence.", 100)));

  const auto outcome = sync.push(hooks);
  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.uploaded, 2);
  EXPECT_EQ(outcome.failed, 0);

  ASSERT_EQ(api.highlightBatches.size(), 1u);
  ASSERT_EQ(api.highlightBatches[0].size(), 2u);
  EXPECT_EQ(api.highlightBatches[0][0].text, "First sentence.");
  EXPECT_EQ(api.highlightBatches[0][0].title, "An Article");
  EXPECT_EQ(api.highlightBatches[0][0].sourceUrl, "https://example.com/a");

  EXPECT_TRUE(pendingIds().empty());

  // Records stay in the file, flagged uploaded, so rendering persists.
  struct Ctx {
    int total = 0;
    int uploaded = 0;
  } ctx;
  ASSERT_TRUE(store.forEachRecord(DOC_A, &ctx, [](void* raw, const HighlightRecord& rec, uint32_t) {
    auto* c = static_cast<Ctx*>(raw);
    c->total++;
    if ((rec.flags & HL_FLAG_UPLOADED) != 0) {
      c->uploaded++;
    }
    return true;
  }));
  EXPECT_EQ(ctx.total, 2);
  EXPECT_EQ(ctx.uploaded, 2);
}

TEST_F(HighlightSyncTest, MissingDocMetadataUploadsTextOnly) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Orphaned text.")));

  const auto outcome = sync.push(hooks);
  EXPECT_TRUE(outcome.ok);
  ASSERT_EQ(api.highlightBatches.size(), 1u);
  EXPECT_EQ(api.highlightBatches[0][0].text, "Orphaned text.");
  EXPECT_EQ(api.highlightBatches[0][0].title, "");
  EXPECT_EQ(api.highlightBatches[0][0].sourceUrl, "");
}

TEST_F(HighlightSyncTest, FailureLeavesRecordsPendingForNextSync) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Not yet.")));
  api.failCreateHighlightsAt = 1;

  const auto outcome = sync.push(hooks);
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.status, ApiStatus::NetworkError);
  EXPECT_EQ(outcome.uploaded, 0);
  EXPECT_EQ(outcome.failed, 1);
  EXPECT_EQ(pendingIds().size(), 1u);

  // Next sync succeeds and drains the queue.
  api.failCreateHighlightsAt = 0;
  const auto retry = sync.push(hooks);
  EXPECT_TRUE(retry.ok);
  EXPECT_EQ(retry.uploaded, 1);
  EXPECT_TRUE(pendingIds().empty());
}

TEST_F(HighlightSyncTest, AlreadyUploadedRecordsAreSkipped) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Old.")));
  ASSERT_TRUE(sync.push(hooks).ok);
  ASSERT_TRUE(store.append(DOC_A, makeRecord("New.", 50)));

  const auto outcome = sync.push(hooks);
  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.uploaded, 1);
  ASSERT_EQ(api.highlightBatches.size(), 2u);
  ASSERT_EQ(api.highlightBatches[1].size(), 1u);
  EXPECT_EQ(api.highlightBatches[1][0].text, "New.");
}

TEST_F(HighlightSyncTest, RateLimitRetriesOnceAfterSleep) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Throttled.")));
  api.highlightsRateLimitFirstN = 1;

  uint32_t slept = 0;
  hooks.ctx = &slept;
  hooks.sleepMs = [](void* ctx, uint32_t ms) { *static_cast<uint32_t*>(ctx) += ms; };

  const auto outcome = sync.push(hooks);
  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.uploaded, 1);
  EXPECT_EQ(slept, 16000u);
  EXPECT_EQ(api.createHighlightsCalls, 2);
}

TEST_F(HighlightSyncTest, PersistentRateLimitAbandonsPass) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Still throttled.")));
  api.highlightsRateLimitFirstN = 2;

  uint32_t slept = 0;
  hooks.ctx = &slept;
  hooks.sleepMs = [](void* ctx, uint32_t ms) { *static_cast<uint32_t*>(ctx) += ms; };

  const auto outcome = sync.push(hooks);
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.status, ApiStatus::RateLimited);
  EXPECT_EQ(pendingIds().size(), 1u);
}

TEST_F(HighlightSyncTest, LargeQueueSplitsIntoBatches) {
  // 20 records exceeds the 16-item batch cap, so two POSTs are required.
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(store.append(DOC_A, makeRecord("Sentence for batch splitting.", i * 100)));
  }

  const auto outcome = sync.push(hooks);
  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.uploaded, 20);
  ASSERT_EQ(api.highlightBatches.size(), 2u);
  EXPECT_EQ(api.highlightBatches[0].size(), 16u);
  EXPECT_EQ(api.highlightBatches[1].size(), 4u);
  EXPECT_TRUE(pendingIds().empty());
}

TEST_F(HighlightSyncTest, MultipleDocumentsAllDrain) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Doc A.")));
  ASSERT_TRUE(store.append(DOC_B, makeRecord("Doc B.")));

  const auto outcome = sync.push(hooks);
  EXPECT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.uploaded, 2);
  EXPECT_TRUE(pendingIds().empty());
}

}  // namespace
