// The one test that actually decides whether this feature works.
//
// ArticleXhtmlWriter's output is consumed by ChapterHtmlSlimParser, which is
// real expat and aborts the whole section build on the first parse error
// (ChapterHtmlSlimParser.cpp:1601). A malformed byte does not degrade the
// article, it erases it. And the input is arbitrary scraped web markup.
//
// So rather than assert on expected output strings -- which would only encode
// what we already believe -- every case here is parsed with the same expat,
// built with the same XML_GE=0 configuration as the firmware. If expat accepts
// it, the device will too.

#include "lib/Readwise/ArticleXhtmlWriter.h"

#include <expat.h>

#include "lib/Readwise/ArticleImageUrl.h"
#include <gtest/gtest.h>

#include <string>
#include <vector>

using readwise::ArticleXhtmlWriter;

namespace {

struct Image {
  size_t index;
  std::string url;
  uint32_t extensionOffset;
};

struct Conversion {
  std::string xhtml;
  std::vector<Image> images;
};

Conversion convert(const std::string& html, const char* baseUrl = "https://example.com/posts/one",
                   const char* title = "A Title", size_t chunkSize = 0) {
  Conversion result;
  ArticleXhtmlWriter::Config config;
  config.baseUrl = baseUrl;
  config.title = title;

  ArticleXhtmlWriter writer(
      [](void* ctx, const char* data, size_t len) {
        static_cast<Conversion*>(ctx)->xhtml.append(data, len);
        return true;
      },
      &result,
      [](void* ctx, size_t index, const char* url, uint32_t extensionOffset) {
        static_cast<Conversion*>(ctx)->images.push_back({index, url, extensionOffset});
        return true;
      },
      &result, config);

  EXPECT_TRUE(writer.begin());
  const size_t step = chunkSize == 0 ? html.size() + 1 : chunkSize;
  for (size_t i = 0; i < html.size(); i += step) {
    const size_t take = std::min(step, html.size() - i);
    EXPECT_TRUE(writer.feed(html.data() + i, take));
  }
  EXPECT_TRUE(writer.finish());
  return result;
}

// Parses with the firmware's expat configuration. Returns an empty string on
// success, or the error description.
std::string parseError(const std::string& xml) {
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (parser == nullptr) {
    return "could not create parser";
  }
  std::string error;
  if (XML_Parse(parser, xml.data(), static_cast<int>(xml.size()), 1) == XML_STATUS_ERROR) {
    error = XML_ErrorString(XML_GetErrorCode(parser));
    error += " at line " + std::to_string(XML_GetCurrentLineNumber(parser)) + " column " +
             std::to_string(XML_GetCurrentColumnNumber(parser));
  }
  XML_ParserFree(parser);
  return error;
}

void expectWellFormed(const std::string& html, const char* what) {
  const Conversion result = convert(html);
  const std::string error = parseError(result.xhtml);
  EXPECT_TRUE(error.empty()) << what << "\ninput:  " << html << "\noutput: " << result.xhtml
                             << "\nexpat:  " << error;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(ArticleXhtmlWriter, ProducesWellFormedXmlFromMalformedHtml) {
  expectWellFormed("<p>unclosed paragraph", "an unclosed block");
  expectWellFormed("<p>one<p>two<p>three", "implied paragraph ends");
  expectWellFormed("<b><i>crossed</b></i>", "mis-nested inline elements");
  expectWellFormed("</div>stray end tag", "an end tag that opened nothing");
  expectWellFormed("<p>text</p></p></p>", "surplus end tags");
  expectWellFormed("<ul><li>a<li>b<li>c</ul>", "implied list item ends");
  expectWellFormed("a < b and 3 > 2 & 4", "raw markup characters in text");
  expectWellFormed("<p>a &bogus; b &amp; c &#8212; d &#x2014;</p>", "undefined and valid entities");
  expectWellFormed("<script>if (a<b) document.write('</p>')</script><p>after</p>", "script content");
  expectWellFormed("<img src=\"x.jpg\" alt='He said \"hi\" & left'>", "quotes in alt text");
  expectWellFormed("", "an empty body");
  expectWellFormed("   \n\t  ", "whitespace only");
  expectWellFormed("<!-- <p>commented</p> --><p>real</p>", "comments");
  expectWellFormed("<!doctype html><html><body><p>x</p></body></html>", "a full document");
}

TEST(ArticleXhtmlWriter, SurvivesNestingDeeperThanTheStack) {
  std::string html;
  for (int i = 0; i < 60; ++i) {
    html += "<div>";
  }
  html += "deep text";
  for (int i = 0; i < 60; ++i) {
    html += "</div>";
  }
  const Conversion result = convert(html);
  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  // Past the stack cap elements go transparent, so the text must still survive.
  EXPECT_TRUE(contains(result.xhtml, "deep text"));
}

TEST(ArticleXhtmlWriter, ScrubsBytesExpatWouldRejectOutright) {
  // Invalid UTF-8 and forbidden control characters are not theoretical:
  // html_content is scraped from the open web, and expat hard-fails on both.
  const std::string html = std::string("<p>bad\x80\xC3 mid\x0B dle\xED\xA0\x80 end\xF5\x80\x80\x80</p>");
  const Conversion result = convert(html);
  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  EXPECT_TRUE(contains(result.xhtml, "bad"));
  EXPECT_TRUE(contains(result.xhtml, "end"));
}

TEST(ArticleXhtmlWriter, KeepsValidMultibyteTextIntactAcrossChunkBoundaries) {
  const std::string html = "<p>caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x98\x80 na\xC3\xAFve</p>";
  const Conversion whole = convert(html);
  EXPECT_TRUE(parseError(whole.xhtml).empty()) << whole.xhtml;
  EXPECT_TRUE(contains(whole.xhtml, "caf\xC3\xA9"));
  EXPECT_TRUE(contains(whole.xhtml, "\xF0\x9F\x98\x80"));

  // A multi-byte character split across two chunks must not be corrupted --
  // chunk boundaries come from TLS record sizes, not character boundaries.
  for (size_t chunk : {static_cast<size_t>(1), static_cast<size_t>(3), static_cast<size_t>(5)}) {
    const Conversion split = convert(html, "https://example.com/posts/one", "A Title", chunk);
    EXPECT_EQ(split.xhtml, whole.xhtml) << "differed at chunk size " << chunk;
  }
}

TEST(ArticleXhtmlWriter, EscapesTheTitleIntoTheHead) {
  const Conversion result = convert("<p>x</p>", "https://example.com/a", "Tom & Jerry <b>");
  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  EXPECT_TRUE(contains(result.xhtml, "<title>Tom &amp; Jerry &lt;b&gt;</title>"));
}

TEST(ArticleXhtmlWriter, RewritesImagesToLocalNamesAndReportsAbsoluteUrls) {
  const Conversion result = convert(
      "<p>a<img src=\"/img/one.jpg\" alt=\"First\">b"
      "<img src=\"../two.png\">c"
      "<img src=\"https://cdn.example.net/three?w=800\">d</p>",
      "https://example.com/posts/one");

  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  ASSERT_EQ(result.images.size(), 3u);
  EXPECT_EQ(result.images[0].url, "https://example.com/img/one.jpg");
  EXPECT_EQ(result.images[1].url, "https://example.com/two.png");
  EXPECT_EQ(result.images[2].url, "https://cdn.example.net/three?w=800");

  EXPECT_TRUE(contains(result.xhtml, "<img src=\"images/0.jpg\" alt=\"First\"/>"));
  EXPECT_TRUE(contains(result.xhtml, "<img src=\"images/1.jpg\" alt=\"\"/>"));
}

// The extension is written before the bytes exist, so the assembler patches it
// once magic-byte sniffing says what the image really is. If these offsets are
// wrong the patch corrupts the markup instead.
TEST(ArticleXhtmlWriter, ReportsExtensionOffsetsThatPointAtTheExtension) {
  const Conversion result =
      convert("<p><img src=\"/a.jpg\"><img src=\"/b.jpg\" alt=\"caption &amp; more\"></p>");

  ASSERT_EQ(result.images.size(), 2u);
  for (const Image& image : result.images) {
    ASSERT_LE(image.extensionOffset + 3, result.xhtml.size());
    EXPECT_EQ(result.xhtml.substr(image.extensionOffset, 3), "jpg")
        << "image " << image.index << " offset " << image.extensionOffset;
  }

  // Patching to PNG must leave the document parseable and the src correct.
  std::string patched = result.xhtml;
  patched.replace(result.images[1].extensionOffset, 3, "png");
  EXPECT_TRUE(parseError(patched).empty()) << patched;
  EXPECT_TRUE(contains(patched, "src=\"images/1.png\""));
  EXPECT_TRUE(contains(patched, "src=\"images/0.jpg\""));
}

TEST(ArticleXhtmlWriter, UnfetchableImagesDegradeToAltTextNotDanglingReferences) {
  const Conversion result = convert(
      "<img src=\"data:image/png;base64,AAAA\" alt=\"Inline\">"
      "<img src=\"/logo.svg\" alt=\"Vector\">"
      "<img src=\"/beacon.jpg\" width=\"1\" height=\"1\">"
      "<img alt=\"No source at all\">");

  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  EXPECT_TRUE(result.images.empty()) << "nothing here is downloadable";

  // An <img> with alt but no src is what ChapterHtmlSlimParser turns into
  // "[Image: alt]", so the reader's own fallback does the work.
  EXPECT_TRUE(contains(result.xhtml, "<img alt=\"Inline\"/>"));
  EXPECT_TRUE(contains(result.xhtml, "<img alt=\"Vector\"/>"));
  EXPECT_TRUE(contains(result.xhtml, "<img alt=\"No source at all\"/>"));
  // A tracking pixel has nothing to say; it should leave no trace.
  EXPECT_FALSE(contains(result.xhtml, "beacon"));
}

TEST(ArticleXhtmlWriter, StopsCollectingImagesAtTheCap) {
  std::string html;
  for (size_t i = 0; i < readwise::MAX_ARTICLE_IMAGES + 5; ++i) {
    html += "<img src=\"/i" + std::to_string(i) + ".jpg\" alt=\"n" + std::to_string(i) + "\">";
  }
  const Conversion result = convert(html);

  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  EXPECT_EQ(result.images.size(), readwise::MAX_ARTICLE_IMAGES);
  // The overflow still reads as an image was here, rather than vanishing.
  EXPECT_TRUE(contains(result.xhtml, "<img alt=\"n24\"/>"));
}

TEST(ArticleXhtmlWriter, PreservesStructureWorthRendering) {
  const Conversion result = convert(
      "<h2>Heading</h2><p>Some <strong>bold</strong> and <em>italic</em>.</p>"
      "<blockquote><p>Quoted</p></blockquote><ul><li>one</li><li>two</li></ul><hr>");

  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  EXPECT_TRUE(contains(result.xhtml, "<h2>Heading</h2>"));
  EXPECT_TRUE(contains(result.xhtml, "<b>bold</b>"));
  EXPECT_TRUE(contains(result.xhtml, "<i>italic</i>"));
  EXPECT_TRUE(contains(result.xhtml, "<blockquote><p>Quoted</p></blockquote>"));
  EXPECT_TRUE(contains(result.xhtml, "<ul><li>one</li><li>two</li></ul>"));
  EXPECT_TRUE(contains(result.xhtml, "<hr/>"));
}

TEST(ArticleXhtmlWriter, DropsTablesAndOtherUnrenderableSubtrees) {
  const Conversion result =
      convert("<p>before</p><table><tr><td>cell</td></tr></table><p>after</p>"
              "<form><input value=\"x\"><button>Go</button></form><p>end</p>");

  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  EXPECT_TRUE(contains(result.xhtml, "before"));
  EXPECT_TRUE(contains(result.xhtml, "after"));
  EXPECT_TRUE(contains(result.xhtml, "end"));
  EXPECT_FALSE(contains(result.xhtml, "cell"));
  EXPECT_FALSE(contains(result.xhtml, "Go"));
}

TEST(ArticleXhtmlWriter, LinksAndSpansAreTransparentSoTheirTextSurvives) {
  const Conversion result = convert("<p><a href=\"/x\">click <span class=\"y\">here</span></a></p>");
  EXPECT_TRUE(parseError(result.xhtml).empty()) << result.xhtml;
  EXPECT_TRUE(contains(result.xhtml, "<p>click here</p>"));
}
