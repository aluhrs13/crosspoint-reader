// Covers the streaming HTML-to-text converter. The property that matters most
// is chunk invariance: output must be identical whether the HTML arrives whole
// or one byte at a time, because the real input is TLS-record-sized fragments
// that split tags, entities, and UTF-8 sequences arbitrarily.

#include <gtest/gtest.h>

#include <string>

#include "lib/Readwise/HtmlTextExtractor.h"

namespace {

using readwise::HtmlTextExtractor;

bool collect(void* ctx, const char* data, size_t len) {
  static_cast<std::string*>(ctx)->append(data, len);
  return true;
}

std::string convert(const std::string& html, size_t chunkSize = 0) {
  std::string out;
  HtmlTextExtractor extractor(&collect, &out);
  if (chunkSize == 0) {
    EXPECT_TRUE(extractor.feed(html.data(), html.size()));
  } else {
    for (size_t i = 0; i < html.size(); i += chunkSize) {
      const size_t len = std::min(chunkSize, html.size() - i);
      EXPECT_TRUE(extractor.feed(html.data() + i, len));
    }
  }
  EXPECT_TRUE(extractor.finish());
  return out;
}

}  // namespace

TEST(HtmlTextExtractor, StripsTagsAndSeparatesBlocks) {
  EXPECT_EQ(convert("<div><p>First paragraph.</p><p>Second one.</p></div>"), "First paragraph.\n\nSecond one.\n");
}

TEST(HtmlTextExtractor, HeadingsAndLineBreaks) {
  EXPECT_EQ(convert("<h1>Title</h1><p>Line one<br>Line two</p>"), "Title\n\nLine one\nLine two\n");
}

TEST(HtmlTextExtractor, ListItemsGetBullets) {
  EXPECT_EQ(convert("<ul><li>alpha</li><li>beta</li></ul>"), "- alpha\n- beta\n");
}

TEST(HtmlTextExtractor, CollapsesWhitespaceLikeHtml) {
  EXPECT_EQ(convert("<p>spaced   out\n\t words</p>"), "spaced out words\n");
}

TEST(HtmlTextExtractor, DecodesEntities) {
  EXPECT_EQ(convert("<p>AT&amp;T &lt;3 &quot;quotes&quot; &#65; &#x42; caf&eacute;</p>"),
            "AT&T <3 \"quotes\" A B caf&eacute;\n")
      << "known entities decode; unknown ones pass through literally";
  EXPECT_EQ(convert("<p>em&mdash;dash&hellip;</p>"),
            "em\xE2\x80\x94"
            "dash\xE2\x80\xA6\n");
}

TEST(HtmlTextExtractor, NbspBecomesPlainSpace) { EXPECT_EQ(convert("<p>a&nbsp;b</p>"), "a b\n"); }

TEST(HtmlTextExtractor, BareAmpersandSurvives) { EXPECT_EQ(convert("<p>fish & chips</p>"), "fish & chips\n"); }

TEST(HtmlTextExtractor, SkipsScriptStyleAndComments) {
  EXPECT_EQ(convert("<p>a</p><script>var x = \"<p>not text</p>\";</script>"
                    "<style>p { color: red; }</style><!-- hidden --><p>b</p>"),
            "a\n\nb\n");
}

TEST(HtmlTextExtractor, QuotedAttributeMayContainAngleBracket) {
  EXPECT_EQ(convert("<p title=\"a > b\">text</p>"), "text\n");
}

TEST(HtmlTextExtractor, RealWorldReadwiseShape) {
  // The rw-email-parsed wrapper observed in the phase-1 probe, comments and
  // all.
  const std::string html =
      "<div class=\"rw-email-parsed\">\n <p>\n  <!-- SPACING TO AVOID BODY TEXT -->\n </p>\n"
      " <p>Actual content here.</p>\n</div>";
  EXPECT_EQ(convert(html), "Actual content here.\n");
}

// The decisive test: byte-at-a-time delivery must produce identical output,
// with boundaries landing inside tags, entities, and multi-byte UTF-8.
TEST(HtmlTextExtractor, OutputIsChunkInvariant) {
  const std::string html =
      "<div><h2>日本語の見出し</h2><p>Body with &amp; entity, em&mdash;dash, "
      "<b>bold</b> and 🚀 emoji.</p><ul><li>ítem uno</li><li>item&nbsp;two</li></ul>"
      "<script>ignore(\"<p>\");</script><p dir=\"rtl\">نص عربي</p></div>";
  const std::string whole = convert(html);
  ASSERT_FALSE(whole.empty());
  for (size_t chunkSize : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{64}}) {
    EXPECT_EQ(convert(html, chunkSize), whole) << "chunk size " << chunkSize << " changed the output";
  }
}

TEST(HtmlTextExtractor, AbortingSinkStopsProcessing) {
  struct Ctx {
    size_t calls = 0;
  } ctx;
  HtmlTextExtractor extractor(
      [](void* c, const char*, size_t) {
        return ++static_cast<Ctx*>(c)->calls < 2;  // refuse the second flush
      },
      &ctx);
  // Enough text to force multiple 256-byte output flushes.
  const std::string big(2048, 'x');
  const bool ok = extractor.feed(big.data(), big.size());
  EXPECT_FALSE(ok && extractor.finish());
}

TEST(HtmlTextExtractor, UnterminatedEntityAtEofEmitsLiterally) {
  EXPECT_EQ(convert("<p>broken &am</p>x &gt"), "broken &am\n\nx &gt\n");
}
