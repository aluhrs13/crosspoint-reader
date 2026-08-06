// buildHighlightsCreateBody tests: JSON shape, escaping, batching, overflow.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ReadwiseClientCore.h"

using namespace readwise;

namespace {

std::string build(const HighlightPayload* items, size_t count) {
  char out[4096];
  if (!buildHighlightsCreateBody(items, count, out, sizeof(out))) {
    return "<FAILED>";
  }
  return out;
}

TEST(HighlightsBodyBuilder, SingleItemWithFullMetadata) {
  const HighlightPayload item{"A sentence.", "An Article", "https://example.com/a"};
  EXPECT_EQ(build(&item, 1),
            "{\"highlights\":[{\"text\":\"A sentence.\","
            "\"title\":\"An Article\","
            "\"source_url\":\"https://example.com/a\","
            "\"source_type\":\"crosspoint\",\"category\":\"articles\"}]}");
}

TEST(HighlightsBodyBuilder, EmptyMetadataFieldsAreOmitted) {
  const HighlightPayload item{"Just text.", "", ""};
  EXPECT_EQ(build(&item, 1),
            "{\"highlights\":[{\"text\":\"Just text.\","
            "\"source_type\":\"crosspoint\",\"category\":\"articles\"}]}");
}

TEST(HighlightsBodyBuilder, MultipleItemsFormAnArray) {
  const HighlightPayload items[] = {{"One.", "", ""}, {"Two.", "", ""}};
  const std::string body = build(items, 2);
  EXPECT_NE(body.find("{\"text\":\"One.\""), std::string::npos);
  EXPECT_NE(body.find("},{\"text\":\"Two.\""), std::string::npos);
}

TEST(HighlightsBodyBuilder, EscapesQuotesBackslashesAndControls) {
  const HighlightPayload item{"He said \"no\\way\"\nthen\tleft.", "Title \"quoted\"", ""};
  const std::string body = build(&item, 1);
  EXPECT_NE(body.find("\\\"no\\\\way\\\""), std::string::npos);
  EXPECT_NE(body.find("\\n"), std::string::npos);
  EXPECT_NE(body.find("\\t"), std::string::npos);
  EXPECT_NE(body.find("\"title\":\"Title \\\"quoted\\\"\""), std::string::npos);
}

TEST(HighlightsBodyBuilder, ControlBytesBecomeUnicodeEscapes) {
  const char raw[] = {'a', 0x01, 'b', '\0'};
  const HighlightPayload item{raw, "", ""};
  const std::string body = build(&item, 1);
  EXPECT_NE(body.find("a\\u0001b"), std::string::npos);
}

TEST(HighlightsBodyBuilder, Utf8PassesThroughUnescaped) {
  const HighlightPayload item{"\xE6\x97\xA5\xE6\x9C\xAC \xE2\x80\x9Cquote\xE2\x80\x9D", "", ""};
  const std::string body = build(&item, 1);
  EXPECT_NE(body.find("\xE6\x97\xA5\xE6\x9C\xAC \xE2\x80\x9Cquote\xE2\x80\x9D"), std::string::npos);
}

TEST(HighlightsBodyBuilder, RejectsEmptyInput) {
  char out[256];
  const HighlightPayload item{"", "T", ""};
  EXPECT_FALSE(buildHighlightsCreateBody(&item, 1, out, sizeof(out)));  // empty text
  EXPECT_FALSE(buildHighlightsCreateBody(nullptr, 1, out, sizeof(out)));
  EXPECT_FALSE(buildHighlightsCreateBody(&item, 0, out, sizeof(out)));
}

TEST(HighlightsBodyBuilder, RejectsBufferTooSmall) {
  const HighlightPayload item{"A reasonably long sentence for overflow.", "Title", "https://example.com"};
  char big[512];
  ASSERT_TRUE(buildHighlightsCreateBody(&item, 1, big, sizeof(big)));
  const size_t needed = strlen(big);
  // Every capacity below the real requirement must fail cleanly, never truncate.
  for (size_t cap = 1; cap <= needed; ++cap) {
    char out[512];
    EXPECT_FALSE(buildHighlightsCreateBody(&item, 1, out, cap)) << "accepted cap " << cap;
  }
}

}  // namespace
