#pragma once

#include <cstddef>
#include <cstdint>

#include "HtmlTokenizer.h"

// Rebuilds a Readwise article's html_content as XHTML the EPUB pipeline can
// parse, and reports the images it references.
//
// The output is consumed by ChapterHtmlSlimParser, which is real expat
// (ChapterHtmlSlimParser.cpp:1556) and aborts the entire section build on the
// first parse error. html_content is arbitrary scraped web markup -- unclosed
// tags, mis-nested inlines, stray end tags, invalid UTF-8, undefined entities.
// So well-formedness here is not a quality goal, it is the difference between
// an article that renders and one that renders as nothing.
//
// It is achieved by construction rather than by validation. An explicit open
// element stack decides what may be emitted: an end tag with no matching open
// element is dropped, an end tag matching an ancestor closes everything above
// it, and finish() closes whatever remains. There is no input that can produce
// unbalanced output.
//
// Images are emitted as <img src="images/<n>.jpg"/> with a *provisional*
// extension. The real format is not known until the bytes arrive, so the byte
// offset of those three characters is reported alongside each URL and patched
// after download. '.jpg' and '.png' are the same length, which is what makes
// the patch a fixed-size overwrite rather than a rewrite.
//
// RAM: the element stack (32 B), a UTF-8 carry of 4 B, and a 256 B output
// buffer. Nothing scales with document size.

namespace readwise {

class ArticleXhtmlWriter : private HtmlTokenHandler {
 public:
  // Return false to abort the conversion.
  using ByteSink = bool (*)(void* ctx, const char* data, size_t len);
  // `extensionOffset` is the byte offset, within the emitted XHTML, of the
  // three extension characters of this image's local filename.
  using ImageSink = bool (*)(void* ctx, size_t index, const char* url, uint32_t extensionOffset);

  struct Config {
    const char* baseUrl = nullptr;  // document source_url, for relative srcs
    const char* title = nullptr;
  };

  // The output vocabulary. Defined in the .cpp alongside the input-to-output
  // rule table; public only so that table can name it.
  enum class TagId : uint8_t;

  ArticleXhtmlWriter(ByteSink out, void* outCtx, ImageSink images, void* imagesCtx, const Config& config);

  // Emits the XML prologue. Must be called before feed().
  bool begin();
  bool feed(const char* data, size_t len);
  // Closes every open element and emits the epilogue.
  bool finish();

  size_t imageCount() const { return imageCount_; }

 private:
  bool onText(const char* data, size_t len, bool literal) override;
  bool onStartTag(const char* name, size_t len, const HtmlAttrCapture* attrs, bool selfClosing) override;
  bool onEndTag(const char* name, size_t len) override;

  bool emitImage(const HtmlAttrCapture& attrs);
  bool openElement(TagId id);
  bool closeElement();
  bool closeInlines();
  bool closeThrough(TagId id);
  bool emitStartTag(TagId id);
  bool emitEndTag(TagId id);

  bool emitEscaped(const char* data, size_t len, bool attributeContext);
  bool emitRaw(const char* data, size_t len);
  bool emitRaw(const char* text);
  bool flush();

  static constexpr size_t STACK_CAP = 32;
  static constexpr size_t OUT_CAP = 256;
  static constexpr size_t TITLE_CAP = 160;

  ByteSink out_;
  void* outCtx_;
  ImageSink images_;
  void* imagesCtx_;
  const char* baseUrl_;
  char title_[TITLE_CAP];

  HtmlAttrCapture capture_;
  HtmlTokenizer tokenizer_;

  TagId stack_[STACK_CAP];
  size_t depth_ = 0;
  // Depth of an element whose entire subtree is discarded (script, table...),
  // counted by occurrences of the same tag so nesting cannot end it early.
  char skipTag_[12] = {};
  size_t skipTagLen_ = 0;
  uint16_t skipDepth_ = 0;

  // Partial UTF-8 sequence carried across a token boundary. The tokenizer
  // flushes text on a fixed buffer size, so a multi-byte character can and
  // does straddle two onText calls.
  uint8_t utf8Carry_[4] = {};
  uint8_t utf8CarryLen_ = 0;
  uint8_t utf8Needed_ = 0;

  size_t imageCount_ = 0;
  uint32_t outOffset_ = 0;
  bool aborted_ = false;

  char buf_[OUT_CAP];
  size_t bufLen_ = 0;
};

}  // namespace readwise
