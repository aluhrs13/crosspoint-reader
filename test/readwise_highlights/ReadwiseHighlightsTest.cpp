// Codec and sentence-scanner tests for lib/Readwise/ReadwiseHighlights.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "lib/Readwise/ReadwiseHighlights.h"

using namespace readwise;

namespace {

HighlightRecord makeRecord(const char* text, uint32_t start = 100, uint32_t end = 200, uint32_t bodySize = 5000,
                           uint8_t flags = 0) {
  HighlightRecord rec;
  rec.flags = flags;
  rec.startByte = start;
  rec.endByte = end;
  rec.bodySize = bodySize;
  rec.textLen = static_cast<uint16_t>(strlen(text));
  memcpy(rec.text, text, rec.textLen);
  rec.text[rec.textLen] = '\0';
  return rec;
}

std::vector<SentenceSpan> scan(const std::string& text, uint32_t baseOffset = 0, size_t maxSpans = 128) {
  std::vector<SentenceSpan> spans;
  scanSentences(text.data(), text.size(), baseOffset, spans, maxSpans);
  return spans;
}

std::string spanText(const std::string& text, const SentenceSpan& span, uint32_t baseOffset = 0) {
  return text.substr(span.start - baseOffset, span.end - span.start);
}

// --- codec ------------------------------------------------------------------

TEST(HighlightCodec, RoundTripsAllFields) {
  const HighlightRecord original = makeRecord("A sentence worth keeping.", 1234, 5678, 90123, HL_FLAG_UPLOADED);

  uint8_t buffer[MAX_ENCODED_HIGHLIGHT];
  const size_t encoded = encodeHighlight(original, buffer, sizeof(buffer));
  ASSERT_GT(encoded, 0u);
  EXPECT_EQ(encoded, 2 + HIGHLIGHT_FIXED_PAYLOAD + original.textLen);

  HighlightRecord decoded;
  size_t consumed = 0;
  ASSERT_TRUE(decodeHighlight(buffer, encoded, decoded, &consumed));
  EXPECT_EQ(consumed, encoded);
  EXPECT_EQ(decoded.flags, HL_FLAG_UPLOADED);
  EXPECT_EQ(decoded.startByte, 1234u);
  EXPECT_EQ(decoded.endByte, 5678u);
  EXPECT_EQ(decoded.bodySize, 90123u);
  EXPECT_STREQ(decoded.text, "A sentence worth keeping.");
}

TEST(HighlightCodec, FlagsSitAtTheDocumentedPatchOffset) {
  const HighlightRecord rec = makeRecord("text", 0, 4, 10, 0x5A);
  uint8_t buffer[MAX_ENCODED_HIGHLIGHT];
  ASSERT_GT(encodeHighlight(rec, buffer, sizeof(buffer)), 0u);
  // markUploaded patches this exact byte in place; the offset is load-bearing.
  EXPECT_EQ(buffer[HIGHLIGHT_FLAGS_OFFSET], 0x5A);
}

TEST(HighlightCodec, RejectsBufferTooSmall) {
  const HighlightRecord rec = makeRecord("0123456789");
  uint8_t buffer[MAX_ENCODED_HIGHLIGHT];
  const size_t needed = 2 + HIGHLIGHT_FIXED_PAYLOAD + rec.textLen;
  EXPECT_EQ(encodeHighlight(rec, buffer, needed - 1), 0u);
  EXPECT_GT(encodeHighlight(rec, buffer, needed), 0u);
}

TEST(HighlightCodec, RejectsTextOverCap) {
  HighlightRecord rec = makeRecord("x");
  rec.textLen = HIGHLIGHT_TEXT_CAP + 1;
  uint8_t buffer[MAX_ENCODED_HIGHLIGHT + 64];
  EXPECT_EQ(encodeHighlight(rec, buffer, sizeof(buffer)), 0u);
}

TEST(HighlightCodec, MaxLengthTextRoundTrips) {
  std::string text(HIGHLIGHT_TEXT_CAP, 'a');
  const HighlightRecord rec = makeRecord(text.c_str());
  uint8_t buffer[MAX_ENCODED_HIGHLIGHT];
  const size_t encoded = encodeHighlight(rec, buffer, sizeof(buffer));
  ASSERT_EQ(encoded, MAX_ENCODED_HIGHLIGHT);
  HighlightRecord decoded;
  ASSERT_TRUE(decodeHighlight(buffer, encoded, decoded));
  EXPECT_EQ(decoded.textLen, HIGHLIGHT_TEXT_CAP);
  EXPECT_EQ(std::string(decoded.text), text);
}

TEST(HighlightCodec, DecodeRejectsTruncatedRecord) {
  const HighlightRecord rec = makeRecord("some sentence text");
  uint8_t buffer[MAX_ENCODED_HIGHLIGHT];
  const size_t encoded = encodeHighlight(rec, buffer, sizeof(buffer));
  ASSERT_GT(encoded, 0u);
  HighlightRecord decoded;
  // Every truncation point must fail cleanly -- this is the torn-append case.
  for (size_t len = 0; len < encoded; ++len) {
    EXPECT_FALSE(decodeHighlight(buffer, len, decoded)) << "accepted truncation at " << len;
  }
}

TEST(HighlightCodec, DecodeRejectsInconsistentLengths) {
  const HighlightRecord rec = makeRecord("abcdef");
  uint8_t buffer[MAX_ENCODED_HIGHLIGHT];
  const size_t encoded = encodeHighlight(rec, buffer, sizeof(buffer));
  ASSERT_GT(encoded, 0u);
  // Corrupt the inner textLen so it disagrees with payloadLen.
  buffer[15] = 0xFF;
  buffer[16] = 0x00;
  HighlightRecord decoded;
  EXPECT_FALSE(decodeHighlight(buffer, encoded, decoded));
}

// --- sentence scanner -------------------------------------------------------

TEST(SentenceScanner, SplitsOnBasicTerminators) {
  const std::string text = "First sentence. Second one! Third here? Done.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 4u);
  EXPECT_EQ(spanText(text, spans[0]), "First sentence.");
  EXPECT_EQ(spanText(text, spans[1]), "Second one!");
  EXPECT_EQ(spanText(text, spans[2]), "Third here?");
  EXPECT_EQ(spanText(text, spans[3]), "Done.");
}

TEST(SentenceScanner, AppliesBaseOffset) {
  const std::string text = "One. Two.";
  const auto spans = scan(text, 1000);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].start, 1000u);
  EXPECT_EQ(spans[0].end, 1004u);
  EXPECT_EQ(spanText(text, spans[1], 1000), "Two.");
}

TEST(SentenceScanner, KeepsTerminatorRunsTogether) {
  const std::string text = "Really?! Yes... Fine.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 3u);
  EXPECT_EQ(spanText(text, spans[0]), "Really?!");
  EXPECT_EQ(spanText(text, spans[1]), "Yes...");
  EXPECT_EQ(spanText(text, spans[2]), "Fine.");
}

TEST(SentenceScanner, IncludesClosingQuotesAndBrackets) {
  const std::string text = "He said \"stop.\" Then (quietly.) More.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 3u);
  EXPECT_EQ(spanText(text, spans[0]), "He said \"stop.\"");
  EXPECT_EQ(spanText(text, spans[1]), "Then (quietly.)");
}

TEST(SentenceScanner, Utf8EllipsisTerminates) {
  const std::string text = "It faded\xE2\x80\xA6 Then returned.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spanText(text, spans[0]), "It faded\xE2\x80\xA6");
  EXPECT_EQ(spanText(text, spans[1]), "Then returned.");
}

TEST(SentenceScanner, CurlyCloseQuoteStaysInSentence) {
  const std::string text = "She said \xE2\x80\x9Cgo.\xE2\x80\x9D Next.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spanText(text, spans[0]), "She said \xE2\x80\x9Cgo.\xE2\x80\x9D");
}

TEST(SentenceScanner, DecimalNumbersDoNotSplit) {
  const std::string text = "Pi is 3.14159 exactly. Next sentence.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spanText(text, spans[0]), "Pi is 3.14159 exactly.");
}

TEST(SentenceScanner, NewlineEndsSpanWithoutTerminator) {
  const std::string text = "A Heading\nBody text follows. More body.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 3u);
  EXPECT_EQ(spanText(text, spans[0]), "A Heading");
  EXPECT_EQ(spanText(text, spans[1]), "Body text follows.");
  EXPECT_EQ(spanText(text, spans[2]), "More body.");
}

TEST(SentenceScanner, ParagraphBreaksProduceNoEmptySpans) {
  const std::string text = "Para one.\n\n\nPara two.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spanText(text, spans[0]), "Para one.");
  EXPECT_EQ(spanText(text, spans[1]), "Para two.");
}

TEST(SentenceScanner, CrLfHandled) {
  const std::string text = "Line one.\r\nLine two.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spanText(text, spans[0]), "Line one.");
  EXPECT_EQ(spanText(text, spans[1]), "Line two.");
}

TEST(SentenceScanner, UnterminatedTailIsASpan) {
  const std::string text = "Complete sentence. And a trailing fragment  ";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spanText(text, spans[1]), "And a trailing fragment");
}

TEST(SentenceScanner, MultiByteUtf8PassesThrough) {
  // CJK text with an ASCII terminator; continuation bytes must never be
  // mistaken for terminators or whitespace.
  const std::string text = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E. Then more.";
  const auto spans = scan(text);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spanText(text, spans[0]), "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.");
}

TEST(SentenceScanner, RespectsMaxSpans) {
  const std::string text = "One. Two. Three. Four. Five.";
  const auto spans = scan(text, 0, 2);
  EXPECT_EQ(spans.size(), 2u);
}

TEST(SentenceScanner, EmptyAndWhitespaceOnlyInputYieldNothing) {
  EXPECT_TRUE(scan("").empty());
  EXPECT_TRUE(scan("   \n\n  \t ").empty());
}

TEST(SentenceScanner, NullBufferIsSafe) {
  std::vector<SentenceSpan> spans;
  scanSentences(nullptr, 10, 0, spans, 8);
  EXPECT_TRUE(spans.empty());
}

}  // namespace
