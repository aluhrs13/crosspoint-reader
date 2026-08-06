#pragma once

#include <cstddef>
#include <cstdint>

// Streaming HTML tokenizer for Readwise article bodies.
//
// Input arrives in arbitrary chunks -- including splits inside a tag name, a
// quoted attribute value, an entity, or a multi-byte UTF-8 sequence -- and
// tokens leave through a handler as they are recognized. Neither the HTML nor
// any single token is ever held whole: total state is a few fixed buffers,
// because a real article body is ~88 KB against a ~380 KB RAM ceiling.
//
// Scope is deliberately below a parser: no element tree, no nesting rules, no
// error recovery. It reports what it sees. Consumers impose structure --
// HtmlTextExtractor flattens to text, ArticleXhtmlWriter rebuilds well-formed
// XHTML.
//
// Entity references are decoded to UTF-8 here, once, so no consumer repeats
// it. Decoded runs are flagged `literal` because they are not source
// whitespace: `&nbsp;` must survive a consumer that collapses runs of spaces.
// Unknown names pass through verbatim rather than being guessed.
//
// <script>, <style>, and <template> bodies are skipped internally; their tags
// are still reported so consumers can track structure.

namespace readwise {

bool htmlEqualsIgnoreCase(const char* a, size_t aLen, const char* b);

// Attribute storage, supplied by consumers that need attribute values. The
// tokenizer parses attributes either way -- a '>' inside a quoted value must
// never end a tag -- but only stores them when given somewhere to put them, so
// text-only consumers pay nothing.
//
// Values are null-terminated and entity-decoded. An over-long value is dropped
// rather than truncated: a half a URL is worse than no URL, and the
// corresponding truncated flag says why it is missing.
struct HtmlAttrCapture {
  static constexpr size_t SRC_CAP = 512;
  static constexpr size_t ALT_CAP = 192;
  static constexpr size_t NUM_CAP = 8;

  char src[SRC_CAP] = {};
  char alt[ALT_CAP] = {};
  char width[NUM_CAP] = {};
  char height[NUM_CAP] = {};
  bool srcTruncated = false;
  bool altTruncated = false;
  bool hasSrcset = false;

  void clear();
};

class HtmlTokenHandler {
 public:
  virtual ~HtmlTokenHandler() = default;

  // `literal` marks text produced by an entity reference. Such text is content
  // even when it looks like whitespace, so consumers that collapse whitespace
  // must not collapse it.
  virtual bool onText(const char* data, size_t len, bool literal) = 0;
  // `attrs` is the capture buffer supplied at construction, or nullptr.
  virtual bool onStartTag(const char* name, size_t len, const HtmlAttrCapture* attrs, bool selfClosing) = 0;
  virtual bool onEndTag(const char* name, size_t len) = 0;
};

class HtmlTokenizer {
 public:
  explicit HtmlTokenizer(HtmlTokenHandler& handler, HtmlAttrCapture* capture = nullptr);

  void reset();
  // Returns false once the handler has refused a token; further input is
  // ignored until reset().
  bool feed(const char* data, size_t len);
  // Flushes buffered text and emits any unterminated entity literally.
  bool finish();

 private:
  enum class State : uint8_t {
    TEXT,
    TAG_OPEN,     // just saw '<'
    TAG_NAME,     // collecting the element name
    BEFORE_ATTR,  // between attributes
    ATTR_NAME,
    AFTER_ATTR_NAME,  // saw a name, waiting to see whether '=' follows
    BEFORE_VALUE,     // saw '=', skipping whitespace
    VALUE_QUOTED,
    VALUE_UNQUOTED,
    SELF_CLOSING,  // saw '/' where '>' was expected next
    ENTITY,        // collecting an &...; reference
    MARKUP_OPEN,   // saw "<!", deciding comment vs declaration
    COMMENT,       // inside <!-- ... -->, ends only at "-->"
    DECLARATION,   // <!doctype ...> or <?...>, ends at the first '>'

    RAWTEXT,        // inside script/style/template content
    RAWTEXT_MAYBE,  // saw '<' inside rawtext, matching against "</name"
  };

  bool consume(char c);
  bool emitTag();
  bool decodeEntity();
  bool putText(char c);
  bool flushText();
  bool emitLiteral(const char* data, size_t len);
  bool emitUtf8Literal(uint32_t codepoint);

  void beginAttr();
  void endAttrName();
  void putAttrValue(char c);
  void finishAttrValue();

  static constexpr size_t TAG_NAME_CAP = 12;
  static constexpr size_t ATTR_NAME_CAP = 12;
  static constexpr size_t ENTITY_CAP = 12;
  static constexpr size_t TEXT_CAP = 128;

  HtmlTokenHandler& handler_;
  HtmlAttrCapture* capture_;

  State state_ = State::TEXT;
  char tagName_[TAG_NAME_CAP];
  size_t tagNameLen_ = 0;
  bool closingTag_ = false;
  bool selfClosing_ = false;
  char quoteChar_ = 0;

  char attrName_[ATTR_NAME_CAP];
  size_t attrNameLen_ = 0;
  // Destination for the value currently being read, or nullptr to discard it.
  char* attrTarget_ = nullptr;
  size_t attrTargetCap_ = 0;
  size_t attrTargetLen_ = 0;
  bool* attrTruncatedFlag_ = nullptr;
  bool attrOverflow_ = false;

  char entity_[ENTITY_CAP];
  size_t entityLen_ = 0;

  // Rawtext bookkeeping: the element whose closing tag ends the raw span, and
  // the match progress while checking a candidate "</name".
  char rawTag_[TAG_NAME_CAP];
  size_t rawTagLen_ = 0;
  size_t rawMatchPos_ = 0;
  // Comment end matching: counts trailing '-' seen.
  uint8_t commentDashes_ = 0;

  bool aborted_ = false;

  char text_[TEXT_CAP];
  size_t textLen_ = 0;
};

}  // namespace readwise
