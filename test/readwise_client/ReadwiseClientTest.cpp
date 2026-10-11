// Covers the host-testable half of the HTTP client -- URL construction, PATCH
// bodies, status mapping, retry-after parsing. The device-only transport
// wiring in HttpReadwiseApi is deliberately thin so that everything
// decision-shaped is exercised here.
//
// The body pipeline moved to ArticleBodyWriter and is covered by
// test/readwise_storage/ArticleAssemblerTest.cpp.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "FakeReadwise.h"
#include "lib/Readwise/ReadwiseClientCore.h"
#include "lib/Readwise/ReadwiseHighlight.h"

namespace {

using namespace readwise;

}  // namespace

TEST(ReadwiseClientCore, ListUrlEncodesTheTimestamp) {
  ListQuery query;
  query.location = Location::Later;
  query.updatedAfter = "2026-08-05T00:39:04.102135+00:00";
  query.limit = 100;

  char url[256];
  ASSERT_TRUE(buildListUrl(query, false, url, sizeof(url)));
  const std::string result = url;
  EXPECT_EQ(result.find('+'), std::string::npos)
      << "an unencoded '+' decodes server-side as a space and silently shifts the sync window";
  EXPECT_NE(result.find("%2B00%3A00"), std::string::npos);
  EXPECT_NE(result.find("location=later"), std::string::npos);
  EXPECT_NE(result.find("limit=100"), std::string::npos);
  EXPECT_EQ(result.find("withHtmlContent"), std::string::npos) << "metadata pages must not request bodies";
}

TEST(ReadwiseClientCore, ListUrlOmitsEmptyParameters) {
  ListQuery query;
  query.limit = 50;
  char url[256];
  ASSERT_TRUE(buildListUrl(query, false, url, sizeof(url)));
  const std::string result = url;
  EXPECT_EQ(result, std::string(API_BASE) + "/list/?limit=50");
}

TEST(ReadwiseClientCore, ListUrlCarriesCursor) {
  ListQuery query;
  query.pageCursor = "01hzzzzzzzzzzzzzzzzzzzzz07";
  char url[256];
  ASSERT_TRUE(buildListUrl(query, false, url, sizeof(url)));
  EXPECT_NE(std::string(url).find("pageCursor=01hzzzzzzzzzzzzzzzzzzzzz07"), std::string::npos);
}

TEST(ReadwiseClientCore, BodyAndUpdateUrls) {
  char url[256];
  ASSERT_TRUE(buildBodyUrl("01hzzzzzzzzzzzzzzzzzzzzz08", url, sizeof(url)));
  EXPECT_EQ(std::string(url), std::string(API_BASE) + "/list/?id=01hzzzzzzzzzzzzzzzzzzzzz08&withHtmlContent=true");

  ASSERT_TRUE(buildUpdateUrl("01hzzzzzzzzzzzzzzzzzzzzz08", url, sizeof(url)));
  EXPECT_EQ(std::string(url), std::string(API_BASE) + "/update/01hzzzzzzzzzzzzzzzzzzzzz08/");

  EXPECT_FALSE(buildBodyUrl(nullptr, url, sizeof(url)));
  EXPECT_FALSE(buildBodyUrl("", url, sizeof(url)));

  char tiny[16];
  EXPECT_FALSE(buildBodyUrl("01hzzzzzzzzzzzzzzzzzzzzz08", tiny, sizeof(tiny)))
      << "overflow must be refused, not truncated into a different URL";
}

TEST(ReadwiseClientCore, UpdateBodies) {
  PendingOp op;
  op.op = OpType::SetLocation;
  op.payload = static_cast<uint8_t>(Location::Archive);
  char body[64];
  ASSERT_TRUE(buildUpdateBody(op, body, sizeof(body)));
  EXPECT_STREQ(body, "{\"location\":\"archive\"}");

  op.op = OpType::SetSeen;
  op.payload = 1;
  ASSERT_TRUE(buildUpdateBody(op, body, sizeof(body)));
  EXPECT_STREQ(body, "{\"seen\":true}");

  // Values that must never reach the wire.
  op.op = OpType::SetLocation;
  op.payload = static_cast<uint8_t>(Location::Unknown);
  EXPECT_FALSE(buildUpdateBody(op, body, sizeof(body)));
  op.payload = static_cast<uint8_t>(Location::Feed);
  EXPECT_FALSE(buildUpdateBody(op, body, sizeof(body)));
}

TEST(ReadwiseClientCore, StatusMapping) {
  EXPECT_EQ(statusFromHttp(200), ApiStatus::Ok);
  EXPECT_EQ(statusFromHttp(204), ApiStatus::Ok);
  EXPECT_EQ(statusFromHttp(401), ApiStatus::AuthFailed);
  EXPECT_EQ(statusFromHttp(403), ApiStatus::AuthFailed);
  EXPECT_EQ(statusFromHttp(429), ApiStatus::RateLimited);
  EXPECT_EQ(statusFromHttp(500), ApiStatus::ServerError);
  EXPECT_EQ(statusFromHttp(503), ApiStatus::ServerError);
  EXPECT_EQ(statusFromHttp(400), ApiStatus::Rejected);
  EXPECT_EQ(statusFromHttp(422), ApiStatus::Rejected);
  EXPECT_EQ(statusFromHttp(404), ApiStatus::NotFound);
  EXPECT_EQ(statusFromHttp(-1), ApiStatus::NetworkError);
}

TEST(ReadwiseClientCore, HttpDetailKeepsAShortServerTrace) {
  char out[80];
  const char* body = "{\"detail\":\"bad\nid\"}\n";
  formatHttpDetail(400, body, strlen(body), out, sizeof(out));
  EXPECT_STREQ(out, "HTTP 400 {\"detail\":\"bad id\"}");

  formatHttpDetail(500, nullptr, 0, out, sizeof(out));
  EXPECT_STREQ(out, "HTTP 500");

  formatHttpDetail(-1, "ignored", 7, out, sizeof(out));
  EXPECT_STREQ(out, "transport");

  char tiny[12];
  formatHttpDetail(400, "{\"detail\":\"long\"}", 16, tiny, sizeof(tiny));
  EXPECT_EQ(strlen(tiny), sizeof(tiny) - 1);
  EXPECT_EQ(tiny[sizeof(tiny) - 1], '\0');
}

TEST(ReadwiseClientCore, RetryAfterParsing) {
  EXPECT_EQ(parseRetryAfter("16"), 16);
  EXPECT_EQ(parseRetryAfter(""), 0);
  EXPECT_EQ(parseRetryAfter(nullptr), 0);
  EXPECT_EQ(parseRetryAfter("soon"), 0);
  EXPECT_EQ(parseRetryAfter("-5"), 0);
  EXPECT_EQ(parseRetryAfter("999999"), 0) << "an implausible value reads as no guidance";
  EXPECT_EQ(parseRetryAfter("16junk"), 0) << "trailing junk means the value cannot be trusted";
  EXPECT_EQ(parseRetryAfter("16 "), 0);
}

TEST(ReadwiseHighlight, BodyEscapesAndNamesTheSource) {
  char body[512];
  ASSERT_TRUE(buildHighlightBody("say \"hello\"\n", "How to Do What You Love", "Paul Graham",
                                 "http://www.paulgraham.com/love.html", body, sizeof(body)));
  EXPECT_STREQ(body,
               "{\"highlights\":[{\"text\":\"say \\\"hello\\\"\\n\",\"title\":\"How to Do What You Love\","
               "\"author\":\"Paul Graham\",\"source_url\":\"http://www.paulgraham.com/love.html\","
               "\"source_type\":\"crosspoint\",\"category\":\"articles\"}]}");
}

TEST(ReadwiseHighlight, BodyOmitsEmptyMetadata) {
  char body[256];
  ASSERT_TRUE(buildHighlightBody("Just the quote", "", nullptr, "", body, sizeof(body)));
  EXPECT_STREQ(body,
               "{\"highlights\":[{\"text\":\"Just the quote\",\"source_type\":\"crosspoint\","
               "\"category\":\"articles\"}]}");
}

TEST(ReadwiseHighlight, FitCutsOnAWordBoundary) {
  char text[64];
  memset(text, 'a', sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  text[4] = ' ';
  ASSERT_TRUE(fitHighlightText(text, 8));
  EXPECT_STREQ(text, "aaaa");
}

TEST(ReadwiseClientCore, DocumentIdValidation) {
  EXPECT_TRUE(isValidDocumentId("01hzzzzzzzzzzzzzzzzzzzzz00"));
  EXPECT_FALSE(isValidDocumentId(nullptr));
  EXPECT_FALSE(isValidDocumentId(""));
  EXPECT_FALSE(isValidDocumentId("../../settings"));
  EXPECT_FALSE(isValidDocumentId("a/b"));
  EXPECT_FALSE(isValidDocumentId("UPPERCASE0123456789012345"));
  EXPECT_FALSE(isValidDocumentId("short"));
}
