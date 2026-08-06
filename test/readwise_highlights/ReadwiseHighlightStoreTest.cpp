// ReadwiseHighlightStore tests against the in-memory FakeFileStore.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "FakeReadwise.h"
#include "lib/Readwise/ReadwiseHighlightStore.h"
#include "lib/Readwise/ReadwiseHighlights.h"

using namespace readwise;
using testing_support::FakeFileStore;

namespace {

constexpr const char* BASE = "/.crosspoint/readwise";
constexpr const char* DOC_A = "01hxyzabcdefghjkmnpqrstvw";
constexpr const char* DOC_B = "01habcdefghjkmnpqrstvwxyz";

HighlightRecord makeRecord(const char* text, uint32_t start, uint32_t end, uint32_t bodySize = 5000) {
  HighlightRecord rec;
  rec.startByte = start;
  rec.endByte = end;
  rec.bodySize = bodySize;
  rec.textLen = static_cast<uint16_t>(strlen(text));
  memcpy(rec.text, text, rec.textLen);
  rec.text[rec.textLen] = '\0';
  return rec;
}

struct CollectCtx {
  std::vector<HighlightRecord> records;
  std::vector<uint32_t> offsets;
  size_t stopAfter = SIZE_MAX;
};

bool collectVisitor(void* ctxRaw, const HighlightRecord& rec, uint32_t offset) {
  auto* ctx = static_cast<CollectCtx*>(ctxRaw);
  ctx->records.push_back(rec);
  ctx->offsets.push_back(offset);
  return ctx->records.size() < ctx->stopAfter;
}

class HighlightStoreTest : public ::testing::Test {
 protected:
  FakeFileStore fake;
  ReadwiseHighlightStore store{fake, BASE};
};

TEST_F(HighlightStoreTest, AppendThenLoadSpansRoundTrips) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("First.", 10, 16)));
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Second.", 40, 47)));

  std::vector<SentenceSpan> spans;
  ASSERT_TRUE(store.loadSpans(DOC_A, 5000, spans));
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].start, 10u);
  EXPECT_EQ(spans[0].end, 16u);
  EXPECT_EQ(spans[1].start, 40u);
  EXPECT_EQ(spans[1].end, 47u);
}

TEST_F(HighlightStoreTest, LoadSpansDropsBodySizeMismatch) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Old body.", 10, 19, 5000)));
  ASSERT_TRUE(store.append(DOC_A, makeRecord("New body.", 20, 29, 6000)));

  std::vector<SentenceSpan> spans;
  ASSERT_TRUE(store.loadSpans(DOC_A, 6000, spans));
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_EQ(spans[0].start, 20u);
}

TEST_F(HighlightStoreTest, MissingFileMeansNoSpansNotError) {
  std::vector<SentenceSpan> spans;
  EXPECT_TRUE(store.loadSpans(DOC_A, 1234, spans));
  EXPECT_TRUE(spans.empty());
}

TEST_F(HighlightStoreTest, ForEachRecordDeliversTextAndOffsets) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Alpha.", 1, 7)));
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Beta.", 9, 14)));

  CollectCtx ctx;
  ASSERT_TRUE(store.forEachRecord(DOC_A, &ctx, collectVisitor));
  ASSERT_EQ(ctx.records.size(), 2u);
  EXPECT_STREQ(ctx.records[0].text, "Alpha.");
  EXPECT_STREQ(ctx.records[1].text, "Beta.");
  EXPECT_EQ(ctx.offsets[0], 1u);  // first record starts right after the version byte
  EXPECT_GT(ctx.offsets[1], ctx.offsets[0]);
}

TEST_F(HighlightStoreTest, VisitorCanStopEarly) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("One.", 0, 4)));
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Two.", 6, 10)));
  CollectCtx ctx;
  ctx.stopAfter = 1;
  ASSERT_TRUE(store.forEachRecord(DOC_A, &ctx, collectVisitor));
  EXPECT_EQ(ctx.records.size(), 1u);
}

TEST_F(HighlightStoreTest, MarkUploadedPatchesOnlyTheFlag) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Persist me.", 5, 16)));

  CollectCtx before;
  ASSERT_TRUE(store.forEachRecord(DOC_A, &before, collectVisitor));
  ASSERT_EQ(before.records.size(), 1u);
  EXPECT_EQ(before.records[0].flags, 0);

  ASSERT_TRUE(store.markUploaded(DOC_A, before.offsets[0]));

  CollectCtx after;
  ASSERT_TRUE(store.forEachRecord(DOC_A, &after, collectVisitor));
  ASSERT_EQ(after.records.size(), 1u);
  EXPECT_EQ(after.records[0].flags, HL_FLAG_UPLOADED);
  EXPECT_STREQ(after.records[0].text, "Persist me.");
  EXPECT_EQ(after.records[0].startByte, 5u);
}

TEST_F(HighlightStoreTest, PendingRegistersOnAppendAndDedups) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("One.", 0, 4)));
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Two.", 6, 10)));
  ASSERT_TRUE(store.append(DOC_B, makeRecord("Other doc.", 0, 10)));

  std::vector<std::array<char, ID_CAP>> ids;
  EXPECT_EQ(store.loadPendingDocIds(ids), 2u);
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_STREQ(ids[0].data(), DOC_A);
  EXPECT_STREQ(ids[1].data(), DOC_B);
}

TEST_F(HighlightStoreTest, RemovePendingDocId) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("One.", 0, 4)));
  ASSERT_TRUE(store.append(DOC_B, makeRecord("Two.", 0, 4)));
  ASSERT_TRUE(store.removePendingDocId(DOC_A));

  std::vector<std::array<char, ID_CAP>> ids;
  EXPECT_EQ(store.loadPendingDocIds(ids), 1u);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_STREQ(ids[0].data(), DOC_B);

  // Removing an id that is not pending is success, not failure.
  EXPECT_TRUE(store.removePendingDocId(DOC_A));
}

TEST_F(HighlightStoreTest, EnforcesPerDocumentCap) {
  for (size_t i = 0; i < MAX_HIGHLIGHTS_PER_DOC; ++i) {
    const auto base = static_cast<uint32_t>(i * 10);
    ASSERT_TRUE(store.append(DOC_A, makeRecord("Filler.", base, base + 7))) << "at " << i;
  }
  EXPECT_FALSE(store.append(DOC_A, makeRecord("One too many.", 9999, 10012)));

  std::vector<SentenceSpan> spans;
  ASSERT_TRUE(store.loadSpans(DOC_A, 5000, spans));
  EXPECT_EQ(spans.size(), MAX_HIGHLIGHTS_PER_DOC);
}

TEST_F(HighlightStoreTest, TornAppendTailIsDiscardedAndRecoverable) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Intact.", 0, 7)));

  // Simulate a power loss mid-append: append a partial record by hand.
  const std::string path = store.highlightsPath(DOC_A);
  std::vector<uint8_t> bytes = fake.files().at(path);
  bytes.push_back(0x50);  // payloadLen lo byte of a record that never finished
  fake.put(path, bytes);

  std::vector<SentenceSpan> spans;
  ASSERT_TRUE(store.loadSpans(DOC_A, 5000, spans));
  ASSERT_EQ(spans.size(), 1u);

  // A subsequent append drops the torn tail and lands cleanly.
  ASSERT_TRUE(store.append(DOC_A, makeRecord("After crash.", 20, 32)));
  spans.clear();
  ASSERT_TRUE(store.loadSpans(DOC_A, 5000, spans));
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[1].start, 20u);
}

TEST_F(HighlightStoreTest, RejectsInvalidDocId) {
  EXPECT_FALSE(store.append("../escape", makeRecord("Nope.", 0, 5)));
  std::vector<SentenceSpan> spans;
  EXPECT_FALSE(store.loadSpans("UPPER", 100, spans));
}

TEST_F(HighlightStoreTest, FailedDurableWriteLeavesOldFileIntact) {
  ASSERT_TRUE(store.append(DOC_A, makeRecord("Survivor.", 0, 9)));

  // Next durable write fails: pending.bin is already registered, so the
  // failing operation is the highlights-file commit.
  fake.failAtWrite(fake.writeCount() + 1);
  EXPECT_FALSE(store.append(DOC_A, makeRecord("Casualty.", 20, 29)));

  std::vector<SentenceSpan> spans;
  ASSERT_TRUE(store.loadSpans(DOC_A, 5000, spans));
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_EQ(spans[0].start, 0u);
}

}  // namespace
