// Covers the four on-disk Readwise formats documented in docs/file-formats.md.
//
// These are pure byte-buffer codecs with no filesystem dependency, so the whole
// format layer is exercised on the host. The properties that matter are that a
// record survives a round trip, that a wrong version byte is rejected rather
// than misread, and that truncated or oversized input is refused rather than
// overrunning a fixed field.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "lib/Readwise/ReadwiseCodec.h"
#include "lib/Readwise/ReadwiseDocument.h"

namespace {

using namespace readwise;

Document makeDocument() {
  Document doc;
  copyBounded(doc.id, ID_CAP, "01hzzzzzzzzzzzzzzzzzzzzz00", 26);
  copyBounded(doc.title, TITLE_CAP, "A Perfectly Ordinary Title", 26);
  copyBounded(doc.author, AUTHOR_CAP, "Some Author", 11);
  copyBounded(doc.siteName, SITE_NAME_CAP, "example.com", 11);
  copyBounded(doc.summary, SUMMARY_CAP, "A short summary.", 16);
  copyBounded(doc.sourceUrl, SOURCE_URL_CAP, "https://example.com/article", 27);
  copyBounded(doc.updatedAt, TIMESTAMP_CAP, "2026-08-05T00:39:05.525412+00:00", 32);
  copyBounded(doc.lastMovedAt, TIMESTAMP_CAP, "2026-08-05T00:39:04.102135+00:00", 32);
  doc.wordCount = 6619;
  doc.location = Location::Shortlist;
  doc.category = Category::Article;
  doc.readingProgressPercent = 42;
  doc.flags = FLAG_SEEN;
  return doc;
}

}  // namespace

TEST(ReadwiseCodec, DocumentRoundTrips) {
  const Document original = makeDocument();
  std::vector<uint8_t> buffer(MAX_ENCODED_RECORD);

  const size_t written = encodeDocument(original, buffer.data(), buffer.size());
  ASSERT_GT(written, 0u);

  Document decoded;
  size_t consumed = 0;
  ASSERT_TRUE(decodeDocument(buffer.data(), written, decoded, &consumed));
  EXPECT_EQ(consumed, written) << "decode must consume exactly what encode produced";

  EXPECT_STREQ(decoded.id, original.id);
  EXPECT_STREQ(decoded.title, original.title);
  EXPECT_STREQ(decoded.author, original.author);
  EXPECT_STREQ(decoded.siteName, original.siteName);
  EXPECT_STREQ(decoded.summary, original.summary);
  EXPECT_STREQ(decoded.sourceUrl, original.sourceUrl);
  EXPECT_STREQ(decoded.updatedAt, original.updatedAt);
  EXPECT_STREQ(decoded.lastMovedAt, original.lastMovedAt);
  EXPECT_EQ(decoded.wordCount, original.wordCount);
  EXPECT_EQ(decoded.location, original.location);
  EXPECT_EQ(decoded.category, original.category);
  EXPECT_EQ(decoded.readingProgressPercent, original.readingProgressPercent);
  EXPECT_EQ(decoded.flags, original.flags);
}

// Every field is a truncation point. Overlong input must be cut to fit and left
// NUL-terminated, never allowed to overrun the fixed field.
TEST(ReadwiseCodec, OverlongFieldsTruncateSafely) {
  Document doc = makeDocument();
  const std::string longTitle(4000, 'x');
  copyBounded(doc.title, TITLE_CAP, longTitle.data(), longTitle.size());
  EXPECT_EQ(strlen(doc.title), static_cast<size_t>(TITLE_CAP - 1));

  std::vector<uint8_t> buffer(MAX_ENCODED_RECORD);
  const size_t written = encodeDocument(doc, buffer.data(), buffer.size());
  ASSERT_GT(written, 0u);

  Document decoded;
  ASSERT_TRUE(decodeDocument(buffer.data(), written, decoded));
  EXPECT_EQ(strlen(decoded.title), static_cast<size_t>(TITLE_CAP - 1));
}

// A record cut short mid-string must be refused, not partially applied.
TEST(ReadwiseCodec, TruncatedRecordIsRejected) {
  const Document original = makeDocument();
  std::vector<uint8_t> buffer(MAX_ENCODED_RECORD);
  const size_t written = encodeDocument(original, buffer.data(), buffer.size());
  ASSERT_GT(written, 4u);

  for (size_t len : {size_t{0}, size_t{1}, written / 2, written - 1}) {
    Document decoded;
    EXPECT_FALSE(decodeDocument(buffer.data(), len, decoded)) << "accepted a record truncated to " << len << " bytes";
  }
}

TEST(ReadwiseCodec, EncodeFailsRatherThanOverrunASmallBuffer) {
  const Document doc = makeDocument();
  std::vector<uint8_t> tiny(8);
  EXPECT_EQ(encodeDocument(doc, tiny.data(), tiny.size()), 0u);
}

TEST(ReadwiseCodec, DocsHeaderRoundTripsAndRejectsWrongVersion) {
  DocsHeader header;
  header.lutOffset = 123456;
  header.recordCount = 100;

  uint8_t buffer[DOCS_HEADER_SIZE] = {};
  ASSERT_EQ(encodeDocsHeader(header, buffer, sizeof(buffer)), DOCS_HEADER_SIZE);

  DocsHeader decoded;
  ASSERT_TRUE(decodeDocsHeader(buffer, sizeof(buffer), decoded));
  EXPECT_EQ(decoded.lutOffset, header.lutOffset);
  EXPECT_EQ(decoded.recordCount, header.recordCount);

  buffer[0] = DOCS_FORMAT_VERSION + 1;
  EXPECT_FALSE(decodeDocsHeader(buffer, sizeof(buffer), decoded))
      << "a future version must be rejected so the cache is rebuilt, not misread";
}

TEST(ReadwiseCodec, IndexRoundTripsAndRejectsWrongVersion) {
  uint8_t header[INDEX_HEADER_SIZE] = {};
  ASSERT_EQ(encodeIndexHeader(3, header, sizeof(header)), INDEX_HEADER_SIZE);

  uint16_t count = 0;
  ASSERT_TRUE(decodeIndexHeader(header, sizeof(header), count));
  EXPECT_EQ(count, 3);

  uint8_t entry[INDEX_ENTRY_SIZE] = {};
  encodeIndexEntry(4242, entry);
  EXPECT_EQ(decodeIndexEntry(entry), 4242);

  header[0] = INDEX_FORMAT_VERSION + 1;
  EXPECT_FALSE(decodeIndexHeader(header, sizeof(header), count));
}

TEST(ReadwiseCodec, JournalEntryRoundTrips) {
  PendingOp op;
  op.seq = 7;
  copyBounded(op.id, ID_CAP, "01hzzzzzzzzzzzzzzzzzzzzz03", 26);
  op.op = OpType::SetLocation;
  op.payload = static_cast<uint8_t>(Location::Archive);
  copyBounded(op.remoteRev, TIMESTAMP_CAP, "2026-08-05T00:39:05.525412+00:00", 32);

  uint8_t buffer[JOURNAL_ENTRY_SIZE] = {};
  encodeJournalEntry(op, buffer);

  PendingOp decoded;
  ASSERT_TRUE(decodeJournalEntry(buffer, sizeof(buffer), decoded));
  EXPECT_EQ(decoded.seq, op.seq);
  EXPECT_STREQ(decoded.id, op.id);
  EXPECT_EQ(decoded.op, op.op);
  EXPECT_EQ(decoded.payload, op.payload);
  EXPECT_STREQ(decoded.remoteRev, op.remoteRev);
}

// The fixed entry width is what makes an interrupted append recoverable, so a
// short buffer must be refused rather than read as a zero-filled operation.
TEST(ReadwiseCodec, PartialJournalEntryIsRejected) {
  PendingOp op;
  op.seq = 1;
  copyBounded(op.id, ID_CAP, "01hzzzzzzzzzzzzzzzzzzzzz04", 26);
  uint8_t buffer[JOURNAL_ENTRY_SIZE] = {};
  encodeJournalEntry(op, buffer);

  PendingOp decoded;
  EXPECT_FALSE(decodeJournalEntry(buffer, JOURNAL_ENTRY_SIZE - 1, decoded));
}

TEST(ReadwiseCodec, UnknownJournalOpIsRejected) {
  uint8_t buffer[JOURNAL_ENTRY_SIZE] = {};
  buffer[4 + ID_CAP] = 200;  // op byte

  PendingOp decoded;
  EXPECT_FALSE(decodeJournalEntry(buffer, sizeof(buffer), decoded))
      << "an unrecognized op must be refused, not executed as SetLocation";
}

TEST(ReadwiseCodec, CheckpointRoundTripsAndRejectsWrongVersion) {
  Checkpoint checkpoint;
  copyBounded(checkpoint.updatedAfter, TIMESTAMP_CAP, "2026-08-05T00:39:05.525412+00:00", 32);
  checkpoint.docCount = 2275;

  uint8_t buffer[CHECKPOINT_SIZE] = {};
  ASSERT_EQ(encodeCheckpoint(checkpoint, buffer, sizeof(buffer)), CHECKPOINT_SIZE);

  Checkpoint decoded;
  ASSERT_TRUE(decodeCheckpoint(buffer, sizeof(buffer), decoded));
  EXPECT_STREQ(decoded.updatedAfter, checkpoint.updatedAfter);
  EXPECT_EQ(decoded.docCount, checkpoint.docCount);

  buffer[0] = CHECKPOINT_FORMAT_VERSION + 1;
  EXPECT_FALSE(decodeCheckpoint(buffer, sizeof(buffer), decoded));
}

// The location set is open: `shortlist` is real but undocumented, and unknown
// values must map to Unknown rather than being guessed at.
TEST(ReadwiseCodec, LocationAndCategoryParsing) {
  EXPECT_EQ(parseLocation("new", 3), Location::New);
  EXPECT_EQ(parseLocation("later", 5), Location::Later);
  EXPECT_EQ(parseLocation("shortlist", 9), Location::Shortlist);
  EXPECT_EQ(parseLocation("archive", 7), Location::Archive);
  EXPECT_EQ(parseLocation("feed", 4), Location::Feed);
  EXPECT_EQ(parseLocation("somethingnew", 12), Location::Unknown);
  EXPECT_EQ(parseLocation(nullptr, 0), Location::Unknown);
  // A prefix of a known value must not match.
  EXPECT_EQ(parseLocation("new", 2), Location::Unknown);

  EXPECT_EQ(parseCategory("rss", 3), Category::Rss);
  EXPECT_EQ(parseCategory("epub", 4), Category::Epub);
  EXPECT_EQ(parseCategory("podcast", 7), Category::Unknown);

  EXPECT_STREQ(locationName(Location::Shortlist), "shortlist");
  EXPECT_STREQ(locationName(Location::Unknown), "unknown");
}
