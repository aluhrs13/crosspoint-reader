#pragma once

#include <cstddef>
#include <cstdint>

#include "HtmlTokenizer.h"

// Streaming HTML-to-plain-text converter for Readwise article bodies.
//
// Tokenizing is HtmlTokenizer's job; this is only the flattening policy on top
// of it. Input arrives in arbitrary chunks and normalized text leaves through a
// caller-supplied sink as it is produced. Neither the HTML nor the text ever
// exists whole in memory: total state is a few fixed buffers, because a real
// article body is ~88 KB against a ~380 KB RAM ceiling.
//
// MVP scope, per docs/readwise-api-contract.md: tags are stripped; block-level
// elements and <br> become newlines; <li> becomes a bulleted line;
// <script>/<style>/<template> content and comments are skipped entirely; the
// common named entities plus numeric references are decoded to UTF-8.
// Everything unrecognized degrades safely -- unknown tags are stripped, unknown
// entities pass through literally.
//
// Whitespace is normalized the way HTML renders it: runs collapse to one
// space, and at most one blank line separates blocks.

namespace readwise {

class HtmlTextExtractor : private HtmlTokenHandler {
 public:
  // Return false to abort; feed() then returns false and further input is
  // ignored until reset().
  using TextSink = bool (*)(void* ctx, const char* data, size_t len);

  HtmlTextExtractor(TextSink sink, void* ctx);

  void reset();
  bool feed(const char* data, size_t len);
  // Flushes buffered output and a trailing newline if any text was emitted.
  bool finish();

 private:
  bool onText(const char* data, size_t len, bool literal) override;
  bool onStartTag(const char* name, size_t len, const HtmlAttrCapture* attrs, bool selfClosing) override;
  bool onEndTag(const char* name, size_t len) override;

  bool handleTag(const char* name, size_t len, bool closing);
  bool put(char c);
  bool flushOutput();
  bool emitPendingBreaks();

  static constexpr size_t OUT_CAP = 256;

  TextSink sink_;
  void* ctx_;
  HtmlTokenizer tokenizer_;

  // Whitespace/break normalization.
  uint8_t pendingNewlines_ = 0;
  bool pendingSpace_ = false;
  bool emittedAny_ = false;
  bool aborted_ = false;

  char out_[OUT_CAP];
  size_t outLen_ = 0;
};

}  // namespace readwise
