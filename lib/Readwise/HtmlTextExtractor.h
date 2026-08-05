#pragma once

#include <cstddef>
#include <cstdint>

// Streaming HTML-to-plain-text converter for Readwise article bodies.
//
// Input arrives in arbitrary chunks -- including splits inside a tag, an
// entity, or a multi-byte UTF-8 sequence -- and normalized text leaves through
// a caller-supplied sink as it is produced. Neither the HTML nor the text ever
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

class HtmlTextExtractor {
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
  enum class State : uint8_t {
    TEXT,
    TAG_OPEN,       // just saw '<'
    TAG_NAME,       // collecting the element name
    TAG_REST,       // inside the tag, waiting for '>'
    TAG_QUOTE,      // inside a quoted attribute value
    ENTITY,         // collecting an &...; reference
    COMMENT,        // inside <!-- ... -->
    RAWTEXT,        // inside script/style/template content
    RAWTEXT_MAYBE,  // saw '<' inside rawtext, matching against "</name"
  };

  bool consume(char c);
  bool handleTagComplete();
  bool decodeEntity();
  bool put(char c);
  bool putUtf8(uint32_t codepoint);
  bool flushOutput();
  bool emitPendingBreaks();

  static constexpr size_t TAG_NAME_CAP = 12;
  static constexpr size_t ENTITY_CAP = 12;
  static constexpr size_t OUT_CAP = 256;

  TextSink sink_;
  void* ctx_;

  State state_ = State::TEXT;
  char tagName_[TAG_NAME_CAP];
  size_t tagNameLen_ = 0;
  bool closingTag_ = false;
  char quoteChar_ = 0;
  char entity_[ENTITY_CAP];
  size_t entityLen_ = 0;

  // Rawtext bookkeeping: the element whose closing tag ends the raw span, and
  // the match progress while checking a candidate "</name".
  char rawTag_[TAG_NAME_CAP];
  size_t rawTagLen_ = 0;
  size_t rawMatchPos_ = 0;
  // Comment end matching: counts trailing '-' seen.
  uint8_t commentDashes_ = 0;

  // Whitespace/break normalization.
  uint8_t pendingNewlines_ = 0;
  bool pendingSpace_ = false;
  bool emittedAny_ = false;
  bool aborted_ = false;

  char out_[OUT_CAP];
  size_t outLen_ = 0;
};

}  // namespace readwise
