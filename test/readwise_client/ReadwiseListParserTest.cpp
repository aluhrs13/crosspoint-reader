// Covers the list-response parser against the sanitized phase-1 fixtures,
// which are captures of real API responses. This is where issue #4's criteria
// for chunk boundaries, escaped Unicode, malformed JSON, oversized content,
// pagination, and partial streams are proven.

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lib/Readwise/ReadwiseListParser.h"

#ifndef READWISE_FIXTURES_DIR
#error "READWISE_FIXTURES_DIR must be defined by the build"
#endif

namespace {

using namespace readwise;

std::string loadFixture(const std::string& name) {
  const std::string path = std::string(READWISE_FIXTURES_DIR) + "/" + name;
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "missing fixture: " << path;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

struct CollectingDocSink : DocumentSink {
  std::vector<Document> docs;
  bool acceptMore = true;
  bool onDocument(const Document& doc) override {
    docs.push_back(doc);
    return acceptMore;
  }
};

struct CollectingBodySink : BodySink {
  std::string body;
  bool complete = false;
  int endCalls = 0;
  bool onBodyChunk(const char* data, size_t len) override {
    body.append(data, len);
    return true;
  }
  bool onBodyEnd(bool wasComplete) override {
    complete = wasComplete;
    ++endCalls;
    return true;
  }
};

bool feedChunked(ReadwiseListParser& parser, const std::string& json, size_t chunkSize) {
  for (size_t i = 0; i < json.size(); i += chunkSize) {
    const size_t len = std::min(chunkSize, json.size() - i);
    if (!parser.feed(json.data() + i, len)) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST(ReadwiseListParser, ParsesRealListResponse) {
  const std::string json = loadFixture("list_normal.json");
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  ASSERT_TRUE(feedChunked(parser, json, 64));
  ASSERT_FALSE(parser.hasError());

  ASSERT_EQ(docs.docs.size(), 5u);
  const Document& first = docs.docs[0];
  EXPECT_STREQ(first.id, "01hzzzzzzzzzzzzzzzzzzzzz00");
  EXPECT_GT(strlen(first.title), 0u);
  EXPECT_GT(first.wordCount, 0u);
  EXPECT_NE(first.location, Location::Unknown);
  EXPECT_GT(strlen(first.updatedAt), 0u);
  EXPECT_EQ(parser.count(), 10000u) << "the saturating count field must still parse";
}

// The event stream must be identical regardless of where TLS record boundaries
// land -- including size 1, which splits every escape and UTF-8 sequence.
TEST(ReadwiseListParser, ChunkBoundariesDoNotChangeResults) {
  const std::string json = loadFixture("list_unicode.json");
  std::vector<std::string> titles;
  for (size_t chunkSize : {json.size(), size_t{1}, size_t{7}, size_t{64}}) {
    CollectingDocSink docs;
    ReadwiseListParser parser(docs, nullptr);
    ASSERT_TRUE(feedChunked(parser, json, chunkSize));
    ASSERT_EQ(docs.docs.size(), 1u);
    titles.push_back(docs.docs[0].title);
  }
  for (size_t i = 1; i < titles.size(); ++i) {
    EXPECT_EQ(titles[i], titles[0]) << "chunking changed a parsed field";
  }
  // CJK from the fixture must survive intact.
  EXPECT_NE(titles[0].find("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"), std::string::npos);
}

TEST(ReadwiseListParser, PaginationCursorRoundTrip) {
  CollectingDocSink docs;
  {
    ReadwiseListParser parser(docs, nullptr);
    ASSERT_TRUE(feedChunked(parser, loadFixture("list_page1.json"), 64));
    EXPECT_TRUE(parser.sawNextPageCursorKey());
    EXPECT_GT(strlen(parser.nextPageCursor()), 0u) << "a non-final page carries a cursor";
  }
  {
    ReadwiseListParser parser(docs, nullptr);
    ASSERT_TRUE(feedChunked(parser, loadFixture("list_page2.json"), 64));
    EXPECT_TRUE(parser.sawNextPageCursorKey());
    EXPECT_EQ(strlen(parser.nextPageCursor()), 0u) << "the final page's null cursor must read as empty";
  }
}

TEST(ReadwiseListParser, EmptyResultIsSuccessNotFailure) {
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  ASSERT_TRUE(feedChunked(parser, loadFixture("list_empty.json"), 64));
  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(docs.docs.size(), 0u);
  EXPECT_EQ(parser.count(), 0u);
}

// The reason this parser exists: an 88 KB html_content must stream to the body
// sink completely, not vanish at the 512-byte token limit.
TEST(ReadwiseListParser, OversizedHtmlContentStreamsCompletely) {
  const std::string json = loadFixture("list_oversized_content.json");
  ASSERT_GT(json.size(), 50u * 1024u);

  CollectingDocSink docs;
  CollectingBodySink body;
  ReadwiseListParser parser(docs, &body);
  ASSERT_TRUE(feedChunked(parser, json, 64));
  ASSERT_FALSE(parser.hasError());

  ASSERT_EQ(docs.docs.size(), 1u);
  EXPECT_EQ(body.endCalls, 1);
  EXPECT_TRUE(body.complete);
  EXPECT_GT(body.body.size(), 50u * 1024u) << "the body must arrive in full, not truncated at TOKEN_BUF_SIZE";
  EXPECT_NE(body.body.find("<div"), std::string::npos) << "body content should be the raw HTML";

  // And it must be chunk-invariant too.
  CollectingBodySink bodyWhole;
  CollectingDocSink docsWhole;
  ReadwiseListParser parserWhole(docsWhole, &bodyWhole);
  ASSERT_TRUE(feedChunked(parserWhole, json, json.size()));
  EXPECT_EQ(body.body, bodyWhole.body);
}

TEST(ReadwiseListParser, EscapedCharactersDecode) {
  const std::string json =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz42\", \"title\": \"Line\\none \\\"quoted\\\" back\\\\slash\","
      "\"location\": \"later\"}]}";
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  ASSERT_TRUE(feedChunked(parser, json, 3));
  ASSERT_EQ(docs.docs.size(), 1u);
  EXPECT_STREQ(docs.docs[0].title, "Line\none \"quoted\" back\\slash");
}

TEST(ReadwiseListParser, UnicodeEscapesDecode) {
  // The live API escapes non-ASCII as \uXXXX; these must decode to UTF-8,
  // including surrogate pairs, across arbitrary chunk boundaries.
  const std::string json =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz46\","
      "\"title\": \"\\u65e5\\u672c\\u8a9e \\ud83d\\ude00 done\", \"location\": \"later\"}]}";
  for (size_t chunkSize : {json.size(), size_t{1}, size_t{5}}) {
    CollectingDocSink docs;
    ReadwiseListParser parser(docs, nullptr);
    ASSERT_TRUE(feedChunked(parser, json, chunkSize));
    ASSERT_EQ(docs.docs.size(), 1u);
    EXPECT_STREQ(docs.docs[0].title, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x98\x80 done")
        << "chunk size " << chunkSize;
  }
}

TEST(ReadwiseListParser, LoneSurrogateBecomesReplacementCharacter) {
  const std::string json =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz47\", \"title\": \"a\\ud800b\", \"location\": \"new\"}]}";
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  ASSERT_TRUE(feedChunked(parser, json, 4));
  ASSERT_EQ(docs.docs.size(), 1u);
  EXPECT_STREQ(docs.docs[0].title,
               "a\xEF\xBF\xBD"
               "b");
}

// An empty html_content is zero chunks then the end marker; the body sink must
// still see onBodyEnd(true), or an empty article reads as a failed transfer.
TEST(ReadwiseListParser, EmptyHtmlContentStillEndsTheBody) {
  const std::string json =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz48\", \"html_content\": \"\", \"location\": \"later\"}]}";
  CollectingDocSink docs;
  CollectingBodySink body;
  ReadwiseListParser parser(docs, &body);
  ASSERT_TRUE(feedChunked(parser, json, 8));
  EXPECT_EQ(body.endCalls, 1);
  EXPECT_TRUE(body.complete);
  EXPECT_TRUE(body.body.empty());
}

// A fully-framed transfer whose JSON was cut short parses without error but
// never closes the envelope; complete() is what the client checks.
TEST(ReadwiseListParser, TruncatedJsonIsNotComplete) {
  const std::string json = loadFixture("list_normal.json");
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  parser.feed(json.data(), json.size() - 5);
  EXPECT_FALSE(parser.complete());

  CollectingDocSink docs2;
  ReadwiseListParser parser2(docs2, nullptr);
  ASSERT_TRUE(feedChunked(parser2, json, 64));
  EXPECT_TRUE(parser2.complete());
}

// first_opened_at drives FLAG_SEEN so the UI never queues a redundant `seen`
// for a document the server already reports opened.
TEST(ReadwiseListParser, FirstOpenedAtSetsSeenFlag) {
  const std::string opened =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz49\","
      "\"first_opened_at\": \"2026-08-05T03:15:27.278000+00:00\", \"location\": \"later\"}]}";
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  ASSERT_TRUE(feedChunked(parser, opened, 16));
  ASSERT_EQ(docs.docs.size(), 1u);
  EXPECT_TRUE(docs.docs[0].flags & FLAG_SEEN);

  const std::string unopened =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz4a\", \"first_opened_at\": null, \"location\": \"later\"}]}";
  CollectingDocSink docs2;
  ReadwiseListParser parser2(docs2, nullptr);
  ASSERT_TRUE(feedChunked(parser2, unopened, 16));
  ASSERT_EQ(docs2.docs.size(), 1u);
  EXPECT_FALSE(docs2.docs[0].flags & FLAG_SEEN);
}

// Ids become SD paths (bodies/<id>.txt); anything not ULID-shaped is refused
// before it can carry path syntax.
TEST(ReadwiseListParser, HostileDocumentIdIsRejected) {
  for (const char* bad : {"../../settings", "a/b", "..", "ABCDEF0123456789ABCDEF0123", ""}) {
    const std::string json = std::string(
                                 "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
                                 "\"id\": \"") +
                             bad + "\", \"location\": \"later\"}]}";
    CollectingDocSink docs;
    ReadwiseListParser parser(docs, nullptr);
    feedChunked(parser, json, 16);
    EXPECT_TRUE(parser.hasError()) << "accepted id: " << bad;
    EXPECT_EQ(docs.docs.size(), 0u) << "delivered doc with id: " << bad;
  }
}

TEST(ReadwiseListParser, MalformedJsonReportsErrorNotDelivery) {
  // A document with no id is refused.
  const std::string noId = "{\"count\": 1, \"results\": [{\"title\": \"orphan\"}]}";
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  EXPECT_FALSE(feedChunked(parser, noId, 16));
  EXPECT_TRUE(parser.hasError());
  EXPECT_EQ(docs.docs.size(), 0u);

  // A bad literal is a parser-level error.
  CollectingDocSink docs2;
  ReadwiseListParser parser2(docs2, nullptr);
  EXPECT_FALSE(feedChunked(parser2, "{\"count\": nqll}", 4));
  EXPECT_TRUE(parser2.hasError());
}

// A stream cut off mid-response delivers what it can and never crashes; the
// client layers on top treat the missing terminator as a failed transfer.
TEST(ReadwiseListParser, PartialStreamIsSafe) {
  const std::string json = loadFixture("list_normal.json");
  for (size_t cut : {json.size() / 4, json.size() / 2, json.size() - 10}) {
    CollectingDocSink docs;
    ReadwiseListParser parser(docs, nullptr);
    parser.feed(json.data(), cut);
    // No assertion on the document count -- only that state stays coherent.
    EXPECT_FALSE(parser.wasAborted());
  }
}

TEST(ReadwiseListParser, AbortingDocumentSinkStopsTheFeed) {
  const std::string json = loadFixture("list_normal.json");
  CollectingDocSink docs;
  docs.acceptMore = false;
  ReadwiseListParser parser(docs, nullptr);
  EXPECT_FALSE(feedChunked(parser, json, 64));
  EXPECT_TRUE(parser.wasAborted());
  EXPECT_EQ(docs.docs.size(), 1u) << "the sink saw exactly the document it refused to continue after";
}

TEST(ReadwiseListParser, TagsObjectIsSkippedStructurally) {
  const std::string json =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz43\","
      "\"tags\": {\"key\": {\"name\": \"deep\", \"title\": \"decoy\"}},"
      "\"title\": \"real title\", \"location\": \"new\"}]}";
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  ASSERT_TRUE(feedChunked(parser, json, 8));
  ASSERT_EQ(docs.docs.size(), 1u);
  EXPECT_STREQ(docs.docs[0].title, "real title") << "a same-named key inside tags must not leak into the document";
  EXPECT_EQ(docs.docs[0].location, Location::New);
}

TEST(ReadwiseListParser, ReadingProgressBecomesPercent) {
  const std::string json =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz44\", \"reading_progress\": 0.42, \"location\": \"later\"}]}";
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  ASSERT_TRUE(feedChunked(parser, json, 16));
  ASSERT_EQ(docs.docs.size(), 1u);
  EXPECT_EQ(docs.docs[0].readingProgressPercent, 42);
}

TEST(ReadwiseListParser, ShortlistLocationParses) {
  // The undocumented location value, present on 16 of 200 real documents.
  const std::string json =
      "{\"count\": 1, \"nextPageCursor\": null, \"results\": [{"
      "\"id\": \"01hzzzzzzzzzzzzzzzzzzzzz45\", \"location\": \"shortlist\"}]}";
  CollectingDocSink docs;
  ReadwiseListParser parser(docs, nullptr);
  ASSERT_TRUE(feedChunked(parser, json, 16));
  ASSERT_EQ(docs.docs.size(), 1u);
  EXPECT_EQ(docs.docs[0].location, Location::Shortlist);
}
