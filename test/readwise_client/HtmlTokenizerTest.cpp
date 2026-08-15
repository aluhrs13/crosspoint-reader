// HtmlTokenizer's contract: the token stream must not depend on how the input
// was chopped up.
//
// This matters because chunk boundaries are decided by TLS record sizes and
// the JSON parser's buffer, so a body can split anywhere -- mid tag name, mid
// quoted attribute, mid entity, mid UTF-8 sequence. A bug there is invisible
// on a fast connection and corrupts articles on a slow one, which is the worst
// possible failure mode to debug on a device.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/Readwise/HtmlTokenizer.h"

using readwise::HtmlAttrCapture;
using readwise::HtmlTokenHandler;
using readwise::HtmlTokenizer;

namespace {

// Renders the token stream as text so two runs can be compared directly, and
// so a failure prints something readable.
class RecordingHandler : public HtmlTokenHandler {
 public:
  std::string log;

  bool onText(const char* data, size_t len, bool literal) override {
    log += literal ? "LIT(" : "TXT(";
    log.append(data, len);
    log += ")\n";
    return true;
  }

  bool onStartTag(const char* name, size_t len, const HtmlAttrCapture* attrs, bool selfClosing) override {
    log += "START(";
    log.append(name, len);
    if (attrs != nullptr) {
      if (attrs->src[0] != '\0') {
        log += " src=";
        log += attrs->src;
      }
      if (attrs->alt[0] != '\0') {
        log += " alt=";
        log += attrs->alt;
      }
      if (attrs->width[0] != '\0') {
        log += " w=";
        log += attrs->width;
      }
      if (attrs->height[0] != '\0') {
        log += " h=";
        log += attrs->height;
      }
      if (attrs->srcTruncated) {
        log += " src-truncated";
      }
      if (attrs->hasSrcset) {
        log += " has-srcset";
      }
    }
    if (selfClosing) {
      log += " /";
    }
    log += ")\n";
    return true;
  }

  bool onEndTag(const char* name, size_t len) override {
    log += "END(";
    log.append(name, len);
    log += ")\n";
    return true;
  }
};

// Adjacent text runs are an artifact of internal buffer flushes, not a
// semantic difference, so they are merged before comparing.
std::string tokenize(const std::string& html, size_t chunkSize, bool captureAttrs = true) {
  RecordingHandler handler;
  HtmlAttrCapture capture;
  HtmlTokenizer tokenizer(handler, captureAttrs ? &capture : nullptr);
  for (size_t i = 0; i < html.size(); i += chunkSize) {
    const size_t take = std::min(chunkSize, html.size() - i);
    EXPECT_TRUE(tokenizer.feed(html.data() + i, take));
  }
  EXPECT_TRUE(tokenizer.finish());
  return handler.log;
}

void expectChunkInvariant(const std::string& html) {
  const std::string whole = tokenize(html, html.empty() ? 1 : html.size());
  for (size_t chunk : {static_cast<size_t>(1), static_cast<size_t>(2), static_cast<size_t>(3), static_cast<size_t>(7),
                       static_cast<size_t>(64)}) {
    EXPECT_EQ(tokenize(html, chunk), whole) << "differed at chunk size " << chunk << "\ninput: " << html;
  }
}

}  // namespace

TEST(HtmlTokenizer, TokenStreamIsIndependentOfChunkBoundaries) {
  expectChunkInvariant("<p>Hello <b>world</b>!</p>");
  expectChunkInvariant("<img src=\"https://example.com/a.jpg\" alt=\"A caption\"/>");
  expectChunkInvariant("Tom &amp; Jerry &mdash; &#8212; &#x2014; &bogus; AT&T");
  expectChunkInvariant("<!-- a > inside a comment --><p>after</p>");
  expectChunkInvariant("<!doctype html><p>x</p>");
  expectChunkInvariant("<script>if (a < b) { document.write('</p>'); }</script><p>safe</p>");
  expectChunkInvariant("caf\xC3\xA9 \xE2\x80\x94 na\xC3\xAFve");
  expectChunkInvariant("<a href='x>y' title=\"a>b\">link</a>");
  expectChunkInvariant("<div class=unquoted id=x>text</div>");
  expectChunkInvariant("unterminated &ent");
  expectChunkInvariant("<p>trailing tag<");
}

TEST(HtmlTokenizer, CapturesTheAttributeAllowlist) {
  EXPECT_EQ(tokenize("<img src=\"a.jpg\" alt=\"Hi\" width=\"640\" height=\"480\">", 64),
            "START(img src=a.jpg alt=Hi w=640 h=480)\n");

  // Unquoted and single-quoted values are equally valid in the wild.
  EXPECT_EQ(tokenize("<img src=a.jpg alt='Hi there'>", 64), "START(img src=a.jpg alt=Hi there)\n");

  // Attributes outside the allowlist are parsed but not stored.
  EXPECT_EQ(tokenize("<img class=\"lead\" src=\"a.jpg\" data-x=\"1\">", 64), "START(img src=a.jpg)\n");

  // srcset is recorded as presence only -- picking a candidate would mean
  // guessing a viewport, and src is the better choice on a 1-bit panel.
  EXPECT_EQ(tokenize("<img src=\"a.jpg\" srcset=\"a-2x.jpg 2x\">", 64), "START(img src=a.jpg has-srcset)\n");
}

TEST(HtmlTokenizer, DecodesEntitiesInAttributeValues) {
  // A query string that survives &amp; is the difference between an image that
  // downloads and one that 404s.
  EXPECT_EQ(tokenize("<img src=\"https://cdn/i?a=1&amp;b=2\">", 64), "START(img src=https://cdn/i?a=1&b=2)\n");
}

TEST(HtmlTokenizer, DropsRatherThanTruncatesAnOverlongSrc) {
  const std::string longUrl = "https://example.com/" + std::string(HtmlAttrCapture::SRC_CAP, 'x') + ".jpg";
  // Half a URL would download the wrong thing or nothing; the flag says why.
  EXPECT_EQ(tokenize("<img src=\"" + longUrl + "\" alt=\"fallback\">", 64), "START(img alt=fallback src-truncated)\n");
}

TEST(HtmlTokenizer, ClearsAttributesBetweenTags) {
  EXPECT_EQ(tokenize("<img src=\"a.jpg\"><img alt=\"only alt\">", 64),
            "START(img src=a.jpg)\nSTART(img alt=only alt)\n");
}

TEST(HtmlTokenizer, ReportsSelfClosingAndEndTags) {
  EXPECT_EQ(tokenize("<br/><hr /><p>x</p>", 64), "START(br /)\nSTART(hr /)\nSTART(p)\nTXT(x)\nEND(p)\n");
}

TEST(HtmlTokenizer, MarksEntityTextLiteralAndSourceTextNot) {
  // The distinction is what lets a consumer collapse source whitespace while
  // keeping &nbsp; as content.
  EXPECT_EQ(tokenize("a&nbsp;b", 64), "TXT(a)\nLIT(\xC2\xA0)\nTXT(b)\n");
  EXPECT_EQ(tokenize("a b", 64), "TXT(a b)\n");
}

TEST(HtmlTokenizer, SkipsRawTextContentButStillReportsItsTags) {
  EXPECT_EQ(tokenize("<style>p { content: '</p>'; }</style>ok", 64), "START(style)\nEND(style)\nTXT(ok)\n");
}

TEST(HtmlTokenizer, AnAbortingHandlerStopsTheFeed) {
  struct Aborting : HtmlTokenHandler {
    int texts = 0;
    bool onText(const char*, size_t, bool) override {
      ++texts;
      return false;
    }
    bool onStartTag(const char*, size_t, const HtmlAttrCapture*, bool) override { return true; }
    bool onEndTag(const char*, size_t) override { return true; }
  } handler;

  HtmlTokenizer tokenizer(handler);
  const std::string html = "one<p>two</p>three";
  EXPECT_FALSE(tokenizer.feed(html.data(), html.size()));
  EXPECT_FALSE(tokenizer.feed(html.data(), html.size())) << "further input must be ignored";
  EXPECT_EQ(handler.texts, 1);
}

TEST(HtmlTokenizer, ResetClearsStateMidTag) {
  RecordingHandler handler;
  HtmlAttrCapture capture;
  HtmlTokenizer tokenizer(handler, &capture);

  const std::string partial = "<img src=\"a.jpg\"";
  EXPECT_TRUE(tokenizer.feed(partial.data(), partial.size()));
  tokenizer.reset();
  handler.log.clear();

  const std::string next = "<p>fresh</p>";
  EXPECT_TRUE(tokenizer.feed(next.data(), next.size()));
  EXPECT_TRUE(tokenizer.finish());
  EXPECT_EQ(handler.log, "START(p)\nTXT(fresh)\nEND(p)\n");
}
