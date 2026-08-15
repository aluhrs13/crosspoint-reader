#include "ArticleXhtmlWriter.h"

#include <cstdio>
#include <cstring>

#include "ArticleImageUrl.h"

namespace readwise {

// Output vocabulary, kept to what ChapterHtmlSlimParser actually styles
// (ChapterHtmlSlimParser.cpp:46-52). Anything outside it is transparent or
// skipped, so the article never depends on markup the reader ignores.
enum class ArticleXhtmlWriter::TagId : uint8_t {
  NONE,
  P,
  H1,
  H2,
  H3,
  H4,
  H5,
  H6,
  BLOCKQUOTE,
  DIV,
  UL,
  OL,
  LI,
  B,
  I,
  BR,
  HR,
};

namespace {

using TagId = ArticleXhtmlWriter::TagId;

// Elements absent from the rule table are transparent: their children flow
// through and nothing is emitted for the element itself. That is the right
// default for <a>, <span>, <font> and the endless supply of unknown tags.
enum class Kind : uint8_t {
  BLOCK,      // may contain blocks; closes an open <p> when opened
  LIST_ITEM,  // like BLOCK, but also closes a sibling <li>
  INLINE,     // closed implicitly whenever a block opens
  VOID,       // never pushed onto the stack
  SKIP,       // element and its entire subtree are discarded
};

struct TagRule {
  const char* name;
  TagId id;
  Kind kind;
};

// Input name -> output element. Several inputs deliberately collapse onto one
// output: <figure> is a <div> and <figcaption> a <p> because the reader has no
// notion of either, but the block structure still reads correctly.
constexpr TagRule kRules[] = {
    {"p", TagId::P, Kind::BLOCK},
    {"div", TagId::DIV, Kind::BLOCK},
    {"section", TagId::DIV, Kind::BLOCK},
    {"article", TagId::DIV, Kind::BLOCK},
    {"main", TagId::DIV, Kind::BLOCK},
    {"header", TagId::DIV, Kind::BLOCK},
    {"footer", TagId::DIV, Kind::BLOCK},
    {"aside", TagId::DIV, Kind::BLOCK},
    {"figure", TagId::DIV, Kind::BLOCK},
    {"figcaption", TagId::P, Kind::BLOCK},
    {"pre", TagId::DIV, Kind::BLOCK},
    {"blockquote", TagId::BLOCKQUOTE, Kind::BLOCK},
    {"h1", TagId::H1, Kind::BLOCK},
    {"h2", TagId::H2, Kind::BLOCK},
    {"h3", TagId::H3, Kind::BLOCK},
    {"h4", TagId::H4, Kind::BLOCK},
    {"h5", TagId::H5, Kind::BLOCK},
    {"h6", TagId::H6, Kind::BLOCK},
    {"ul", TagId::UL, Kind::BLOCK},
    {"ol", TagId::OL, Kind::BLOCK},
    {"li", TagId::LI, Kind::LIST_ITEM},

    {"b", TagId::B, Kind::INLINE},
    {"strong", TagId::B, Kind::INLINE},
    {"i", TagId::I, Kind::INLINE},
    {"em", TagId::I, Kind::INLINE},
    {"cite", TagId::I, Kind::INLINE},

    {"br", TagId::BR, Kind::VOID},
    {"hr", TagId::HR, Kind::VOID},

    // Dropped wholesale. <table> goes here because emitting it would require
    // guaranteeing tr/td nesting too, and a table-layout newsletter flattened
    // into runs reads worse than its text alone.
    {"script", TagId::NONE, Kind::SKIP},
    {"style", TagId::NONE, Kind::SKIP},
    {"template", TagId::NONE, Kind::SKIP},
    {"noscript", TagId::NONE, Kind::SKIP},
    {"svg", TagId::NONE, Kind::SKIP},
    {"iframe", TagId::NONE, Kind::SKIP},
    {"form", TagId::NONE, Kind::SKIP},
    {"button", TagId::NONE, Kind::SKIP},
    {"select", TagId::NONE, Kind::SKIP},
    {"table", TagId::NONE, Kind::SKIP},
};

const TagRule* findRule(const char* name, size_t len) {
  for (const TagRule& rule : kRules) {
    if (htmlEqualsIgnoreCase(name, len, rule.name)) {
      return &rule;
    }
  }
  return nullptr;
}

const char* tagName(TagId id) {
  switch (id) {
    case TagId::P:
      return "p";
    case TagId::H1:
      return "h1";
    case TagId::H2:
      return "h2";
    case TagId::H3:
      return "h3";
    case TagId::H4:
      return "h4";
    case TagId::H5:
      return "h5";
    case TagId::H6:
      return "h6";
    case TagId::BLOCKQUOTE:
      return "blockquote";
    case TagId::DIV:
      return "div";
    case TagId::UL:
      return "ul";
    case TagId::OL:
      return "ol";
    case TagId::LI:
      return "li";
    case TagId::B:
      return "b";
    case TagId::I:
      return "i";
    case TagId::BR:
      return "br";
    case TagId::HR:
      return "hr";
    case TagId::NONE:
      break;
  }
  return "";
}

bool isInline(TagId id) { return id == TagId::B || id == TagId::I; }

// XML 1.0 forbids most C0 controls outright; tab, newline, and carriage return
// are the only ones permitted.
bool isForbiddenControl(unsigned char c) { return c < 0x20 && c != '\t' && c != '\n' && c != '\r'; }

uint8_t utf8SequenceLength(unsigned char lead) {
  if (lead < 0x80) {
    return 1;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    return 2;
  }
  if (lead >= 0xE0 && lead <= 0xEF) {
    return 3;
  }
  if (lead >= 0xF0 && lead <= 0xF4) {
    return 4;
  }
  return 0;  // continuation byte out of place, or 0xC0/0xC1/0xF5+
}

// Rejects overlongs, surrogates, and anything past U+10FFFF. Expat fails hard
// on all of them, and html_content is scraped from the open web.
bool isValidSequence(const uint8_t* bytes, uint8_t len) {
  for (uint8_t i = 1; i < len; ++i) {
    if ((bytes[i] & 0xC0) != 0x80) {
      return false;
    }
  }
  uint32_t codepoint;
  switch (len) {
    case 1:
      return true;
    case 2:
      codepoint = static_cast<uint32_t>(bytes[0] & 0x1F) << 6 | (bytes[1] & 0x3F);
      return codepoint >= 0x80;
    case 3:
      codepoint = static_cast<uint32_t>(bytes[0] & 0x0F) << 12 | static_cast<uint32_t>(bytes[1] & 0x3F) << 6 |
                  (bytes[2] & 0x3F);
      return codepoint >= 0x800 && (codepoint < 0xD800 || codepoint > 0xDFFF);
    case 4:
      codepoint = static_cast<uint32_t>(bytes[0] & 0x07) << 18 | static_cast<uint32_t>(bytes[1] & 0x3F) << 12 |
                  static_cast<uint32_t>(bytes[2] & 0x3F) << 6 | (bytes[3] & 0x3F);
      return codepoint >= 0x10000 && codepoint <= 0x10FFFF;
    default:
      return false;
  }
}

}  // namespace

ArticleXhtmlWriter::ArticleXhtmlWriter(ByteSink out, void* outCtx, ImageSink images, void* imagesCtx,
                                       const Config& config)
    : out_(out),
      outCtx_(outCtx),
      images_(images),
      imagesCtx_(imagesCtx),
      baseUrl_(config.baseUrl),
      tokenizer_(*this, &capture_) {
  title_[0] = '\0';
  if (config.title != nullptr) {
    const size_t len = strnlen(config.title, TITLE_CAP - 1);
    memcpy(title_, config.title, len);
    title_[len] = '\0';
  }
}

bool ArticleXhtmlWriter::begin() {
  if (!emitRaw("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>")) {
    return false;
  }
  // The title comes from the API, not the body, but it is still arbitrary text
  // and sits inside an element.
  if (!emitEscaped(title_, strlen(title_), false)) {
    return false;
  }
  return emitRaw("</title></head><body>");
}

bool ArticleXhtmlWriter::feed(const char* data, size_t len) {
  if (aborted_) {
    return false;
  }
  if (!tokenizer_.feed(data, len)) {
    aborted_ = true;
    return false;
  }
  return true;
}

bool ArticleXhtmlWriter::finish() {
  if (aborted_ || !tokenizer_.finish()) {
    return false;
  }
  // Whatever the input left open, the output does not.
  while (depth_ > 0) {
    if (!closeElement()) {
      return false;
    }
  }
  if (!emitRaw("</body></html>\n")) {
    return false;
  }
  return flush();
}

bool ArticleXhtmlWriter::onText(const char* data, size_t len, bool) {
  if (skipDepth_ > 0) {
    return true;
  }
  // Text outside any element would be a stray child of <body>, which is legal
  // XHTML, so it is emitted as-is; the reader treats it as an anonymous block.
  return emitEscaped(data, len, false);
}

bool ArticleXhtmlWriter::onStartTag(const char* name, size_t len, const HtmlAttrCapture* attrs, bool selfClosing) {
  if (skipDepth_ > 0) {
    // Counting only same-name opens means a nested <table> inside a skipped
    // <table> cannot end the skip early.
    if (htmlEqualsIgnoreCase(name, len, skipTag_)) {
      ++skipDepth_;
    }
    return true;
  }

  if (htmlEqualsIgnoreCase(name, len, "img") || htmlEqualsIgnoreCase(name, len, "image")) {
    return attrs != nullptr ? emitImage(*attrs) : true;
  }

  const TagRule* rule = findRule(name, len);
  if (rule == nullptr) {
    // Unknown or purely presentational (<a>, <span>, <font>): children flow
    // through so no text is lost.
    return true;
  }

  switch (rule->kind) {
    case Kind::SKIP:
      // A self-closing <svg/> opens nothing, so it must not start a skip.
      if (!selfClosing) {
        memcpy(skipTag_, name, len < sizeof(skipTag_) ? len : sizeof(skipTag_) - 1);
        skipTagLen_ = len < sizeof(skipTag_) ? len : sizeof(skipTag_) - 1;
        skipTag_[skipTagLen_] = '\0';
        skipDepth_ = 1;
      }
      return true;

    case Kind::VOID:
      return emitStartTag(rule->id);

    case Kind::LIST_ITEM:
      // A new <li> ends the previous one; browsers imply this and authors rely
      // on it.
      if (!closeInlines() || (depth_ > 0 && stack_[depth_ - 1] == TagId::LI && !closeElement())) {
        return false;
      }
      return openElement(rule->id);

    case Kind::BLOCK:
      // A block cannot live inside <p> or inside emphasis.
      if (!closeInlines()) {
        return false;
      }
      if (depth_ > 0 && stack_[depth_ - 1] == TagId::P && !closeElement()) {
        return false;
      }
      return openElement(rule->id);

    case Kind::INLINE:
      return openElement(rule->id);
  }
  return true;
}

bool ArticleXhtmlWriter::onEndTag(const char* name, size_t len) {
  if (skipDepth_ > 0) {
    if (htmlEqualsIgnoreCase(name, len, skipTag_) && --skipDepth_ == 0) {
      skipTagLen_ = 0;
      skipTag_[0] = '\0';
    }
    return true;
  }

  const TagRule* rule = findRule(name, len);
  if (rule == nullptr || rule->id == TagId::NONE || rule->kind == Kind::VOID) {
    return true;
  }
  return closeThrough(rule->id);
}

bool ArticleXhtmlWriter::emitImage(const HtmlAttrCapture& attrs) {
  char url[IMAGE_URL_CAP];
  ImageUrlResult result = ImageUrlResult::NoSource;
  if (imageCount_ < MAX_ARTICLE_IMAGES) {
    result = resolveArticleImageUrl(baseUrl_, attrs.src, attrs.width, attrs.height, url, sizeof(url));
  }

  if (result != ImageUrlResult::Accepted) {
    // A tracking pixel with no alt is noise; drop it silently.
    if (attrs.alt[0] == '\0') {
      return true;
    }
    // An <img> carrying alt but no src is exactly what ChapterHtmlSlimParser
    // turns into "[Image: alt]" (ChapterHtmlSlimParser.cpp:601, :843), so the
    // degradation path is the one the reader already implements.
    if (!emitRaw("<img alt=\"") || !emitEscaped(attrs.alt, strlen(attrs.alt), true)) {
      return false;
    }
    return emitRaw("\"/>");
  }

  // The extension is provisional: only the downloaded bytes can say whether
  // this is a JPEG or a PNG, and by then the XHTML is already written. Both
  // spellings are three characters, so the assembler patches in place.
  char reference[48];
  const int written =
      snprintf(reference, sizeof(reference), "<img src=\"images/%u.jpg\" alt=\"", static_cast<unsigned>(imageCount_));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(reference)) {
    return true;
  }
  const char* extension = strstr(reference, ".jpg");
  const uint32_t extensionOffset = outOffset_ + static_cast<uint32_t>(extension - reference) + 1;

  if (!emitRaw(reference, static_cast<size_t>(written)) || !emitEscaped(attrs.alt, strlen(attrs.alt), true) ||
      !emitRaw("\"/>")) {
    return false;
  }

  if (images_ != nullptr && !images_(imagesCtx_, imageCount_, url, extensionOffset)) {
    aborted_ = true;
    return false;
  }
  ++imageCount_;
  return true;
}

bool ArticleXhtmlWriter::openElement(TagId id) {
  if (depth_ >= STACK_CAP) {
    // Past the cap the element becomes transparent rather than unbalanced:
    // its text still reaches the reader, only its styling is lost.
    return true;
  }
  if (!emitStartTag(id)) {
    return false;
  }
  stack_[depth_++] = id;
  return true;
}

bool ArticleXhtmlWriter::closeElement() {
  if (depth_ == 0) {
    return true;
  }
  return emitEndTag(stack_[--depth_]);
}

bool ArticleXhtmlWriter::closeInlines() {
  while (depth_ > 0 && isInline(stack_[depth_ - 1])) {
    if (!closeElement()) {
      return false;
    }
  }
  return true;
}

// Closes up to and including the nearest matching ancestor. A stray end tag
// with no match is dropped rather than closing something it did not open --
// "</div>" in the middle of a paragraph must not end the paragraph.
bool ArticleXhtmlWriter::closeThrough(TagId id) {
  size_t found = depth_;
  for (size_t i = depth_; i > 0; --i) {
    if (stack_[i - 1] == id) {
      found = i - 1;
      break;
    }
  }
  if (found == depth_) {
    return true;
  }
  while (depth_ > found) {
    if (!closeElement()) {
      return false;
    }
  }
  return true;
}

bool ArticleXhtmlWriter::emitStartTag(TagId id) {
  const char* name = tagName(id);
  if (name[0] == '\0') {
    return true;
  }
  const bool isVoid = id == TagId::BR || id == TagId::HR;
  return emitRaw("<") && emitRaw(name) && emitRaw(isVoid ? "/>" : ">");
}

bool ArticleXhtmlWriter::emitEndTag(TagId id) {
  const char* name = tagName(id);
  if (name[0] == '\0') {
    return true;
  }
  return emitRaw("</") && emitRaw(name) && emitRaw(">");
}

bool ArticleXhtmlWriter::emitEscaped(const char* data, size_t len, bool attributeContext) {
  for (size_t i = 0; i < len; ++i) {
    const uint8_t byte = static_cast<uint8_t>(data[i]);

    // Reassemble any multi-byte sequence split across token boundaries before
    // judging it.
    if (utf8Needed_ > 0) {
      if ((byte & 0xC0) != 0x80) {
        utf8CarryLen_ = 0;
        utf8Needed_ = 0;
        // Not a continuation: fall through and reconsider this byte as a lead.
      } else {
        utf8Carry_[utf8CarryLen_++] = byte;
        if (--utf8Needed_ > 0) {
          continue;
        }
        if (isValidSequence(utf8Carry_, utf8CarryLen_) &&
            !emitRaw(reinterpret_cast<const char*>(utf8Carry_), utf8CarryLen_)) {
          return false;
        }
        utf8CarryLen_ = 0;
        continue;
      }
    }

    if (byte < 0x80) {
      if (isForbiddenControl(byte) || byte == 0x7F) {
        continue;  // XML 1.0 has no representation for these at all.
      }
      const char c = static_cast<char>(byte);
      bool ok = true;
      if (c == '&') {
        ok = emitRaw("&amp;");
      } else if (c == '<') {
        ok = emitRaw("&lt;");
      } else if (c == '>') {
        ok = emitRaw("&gt;");
      } else if (c == '"' && attributeContext) {
        ok = emitRaw("&quot;");
      } else if ((c == '\n' || c == '\t' || c == '\r') && attributeContext) {
        // Attribute-value normalization would turn these into spaces anyway;
        // doing it here keeps alt text on one line.
        ok = emitRaw(" ");
      } else {
        ok = emitRaw(&c, 1);
      }
      if (!ok) {
        return false;
      }
      continue;
    }

    const uint8_t sequenceLength = utf8SequenceLength(byte);
    if (sequenceLength == 0) {
      continue;  // stray continuation byte or an invalid lead
    }
    if (sequenceLength == 1) {
      if (!emitRaw(reinterpret_cast<const char*>(&byte), 1)) {
        return false;
      }
      continue;
    }
    utf8Carry_[0] = byte;
    utf8CarryLen_ = 1;
    utf8Needed_ = static_cast<uint8_t>(sequenceLength - 1);
  }
  return true;
}

bool ArticleXhtmlWriter::emitRaw(const char* text) { return emitRaw(text, strlen(text)); }

bool ArticleXhtmlWriter::emitRaw(const char* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    buf_[bufLen_++] = data[i];
    if (bufLen_ == OUT_CAP && !flush()) {
      return false;
    }
  }
  outOffset_ += static_cast<uint32_t>(len);
  return true;
}

bool ArticleXhtmlWriter::flush() {
  if (bufLen_ == 0) {
    return true;
  }
  const size_t len = bufLen_;
  bufLen_ = 0;
  if (out_ != nullptr && !out_(outCtx_, buf_, len)) {
    aborted_ = true;
    return false;
  }
  return true;
}

}  // namespace readwise
