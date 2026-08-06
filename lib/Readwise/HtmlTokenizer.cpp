#include "HtmlTokenizer.h"

#include <cstring>

namespace readwise {
namespace {

char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool isRawTextTag(const char* name, size_t len) {
  return htmlEqualsIgnoreCase(name, len, "script") || htmlEqualsIgnoreCase(name, len, "style") ||
         htmlEqualsIgnoreCase(name, len, "template");
}

struct NamedEntity {
  const char* name;
  uint32_t codepoint;
};

// The set observed in real Reader html_content plus the HTML basics. Unknown
// names pass through literally rather than being guessed.
constexpr NamedEntity kEntities[] = {
    {"amp", '&'},      {"lt", '<'},       {"gt", '>'},        {"quot", '"'},      {"apos", '\''},
    {"nbsp", 0x00A0},  {"mdash", 0x2014}, {"ndash", 0x2013},  {"hellip", 0x2026}, {"rsquo", 0x2019},
    {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C},  {"copy", 0x00A9},   {"reg", 0x00AE},
    {"trade", 0x2122}, {"deg", 0x00B0},   {"middot", 0x00B7}, {"bull", 0x2022},   {"times", 0x00D7},
};

// Resolves a reference body (what sits between '&' and ';') to a codepoint.
// Returns false for anything unrecognized or malformed, which the callers
// treat as "not a reference at all".
bool resolveEntity(const char* name, size_t len, uint32_t* codepoint) {
  if (len == 0) {
    return false;
  }
  if (name[0] == '#') {
    uint32_t value = 0;
    const bool hex = len > 2 && (name[1] == 'x' || name[1] == 'X');
    const size_t start = hex ? 2 : 1;
    if (start >= len) {
      return false;
    }
    for (size_t i = start; i < len; ++i) {
      const char c = name[i];
      uint32_t digit;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint32_t>(c - '0');
      } else if (hex && c >= 'a' && c <= 'f') {
        digit = static_cast<uint32_t>(c - 'a' + 10);
      } else if (hex && c >= 'A' && c <= 'F') {
        digit = static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
      value = value * (hex ? 16 : 10) + digit;
      if (value > 0x10FFFF) {
        return false;
      }
    }
    if (value == 0) {
      return false;
    }
    *codepoint = value;
    return true;
  }
  for (const NamedEntity& entry : kEntities) {
    if (htmlEqualsIgnoreCase(name, len, entry.name)) {
      *codepoint = entry.codepoint;
      return true;
    }
  }
  return false;
}

size_t encodeUtf8(uint32_t codepoint, char* out) {
  if (codepoint < 0x80) {
    out[0] = static_cast<char>(codepoint);
    return 1;
  }
  if (codepoint < 0x800) {
    out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
    out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 2;
  }
  if (codepoint < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
    out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
  out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
  return 4;
}

// Decodes references inside an attribute value, in place. Safe because every
// reference is at least 4 bytes ("&lt;") and decodes to at most 4, so the
// result can never outgrow the source.
//
// This is not cosmetic: a src of "?a=1&amp;b=2" fetched verbatim is a
// different URL from the one the page meant, and usually a 404.
size_t decodeAttrEntities(char* buf, size_t len) {
  size_t out = 0;
  size_t i = 0;
  while (i < len) {
    if (buf[i] != '&') {
      buf[out++] = buf[i++];
      continue;
    }
    size_t end = i + 1;
    while (end < len && end - i <= 10 && buf[end] != ';' && buf[end] != '&' && buf[end] != ' ') {
      ++end;
    }
    uint32_t codepoint = 0;
    if (end < len && buf[end] == ';' && resolveEntity(buf + i + 1, end - i - 1, &codepoint)) {
      out += encodeUtf8(codepoint, buf + out);
      i = end + 1;
      continue;
    }
    buf[out++] = buf[i++];
  }
  return out;
}

}  // namespace

bool htmlEqualsIgnoreCase(const char* a, size_t aLen, const char* b) {
  const size_t bLen = strlen(b);
  if (aLen != bLen) {
    return false;
  }
  for (size_t i = 0; i < aLen; ++i) {
    if (toLowerAscii(a[i]) != b[i]) {
      return false;
    }
  }
  return true;
}

void HtmlAttrCapture::clear() {
  src[0] = '\0';
  alt[0] = '\0';
  width[0] = '\0';
  height[0] = '\0';
  srcTruncated = false;
  altTruncated = false;
  hasSrcset = false;
}

HtmlTokenizer::HtmlTokenizer(HtmlTokenHandler& handler, HtmlAttrCapture* capture)
    : handler_(handler), capture_(capture) {
  reset();
}

void HtmlTokenizer::reset() {
  state_ = State::TEXT;
  tagNameLen_ = 0;
  closingTag_ = false;
  selfClosing_ = false;
  quoteChar_ = 0;
  attrNameLen_ = 0;
  attrTarget_ = nullptr;
  attrTargetCap_ = 0;
  attrTargetLen_ = 0;
  attrTruncatedFlag_ = nullptr;
  attrOverflow_ = false;
  entityLen_ = 0;
  rawTagLen_ = 0;
  rawMatchPos_ = 0;
  commentDashes_ = 0;
  aborted_ = false;
  textLen_ = 0;
  if (capture_ != nullptr) {
    capture_->clear();
  }
}

bool HtmlTokenizer::feed(const char* data, size_t len) {
  if (aborted_) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    if (!consume(data[i])) {
      aborted_ = true;
      return false;
    }
  }
  return true;
}

bool HtmlTokenizer::finish() {
  if (aborted_) {
    return false;
  }
  // An unterminated entity at end of input is content, not a reference.
  if (state_ == State::ENTITY) {
    if (!flushText() || !emitLiteral("&", 1) || !emitLiteral(entity_, entityLen_)) {
      return false;
    }
    state_ = State::TEXT;
    entityLen_ = 0;
  }
  return flushText();
}

bool HtmlTokenizer::consume(char c) {
  switch (state_) {
    case State::TEXT:
      if (c == '<') {
        if (!flushText()) {
          return false;
        }
        state_ = State::TAG_OPEN;
        tagNameLen_ = 0;
        closingTag_ = false;
        selfClosing_ = false;
        if (capture_ != nullptr) {
          capture_->clear();
        }
        return true;
      }
      if (c == '&') {
        if (!flushText()) {
          return false;
        }
        state_ = State::ENTITY;
        entityLen_ = 0;
        return true;
      }
      return putText(c);

    case State::TAG_OPEN:
      if (c == '/') {
        closingTag_ = true;
        return true;
      }
      if (c == '!') {
        // "<!--" opens a comment (ends only at "-->"); anything else after
        // "<!" is a declaration ending at the first '>'.
        state_ = State::MARKUP_OPEN;
        commentDashes_ = 0;
        return true;
      }
      if (c == '?') {
        state_ = State::DECLARATION;
        return true;
      }
      state_ = State::TAG_NAME;
      [[fallthrough]];

    case State::TAG_NAME:
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        if (tagNameLen_ < TAG_NAME_CAP - 1) {
          tagName_[tagNameLen_++] = toLowerAscii(c);
        }
        return true;
      }
      if (c == '>') {
        return emitTag();
      }
      if (c == '/') {
        state_ = State::SELF_CLOSING;
        return true;
      }
      state_ = State::BEFORE_ATTR;
      return true;

    case State::BEFORE_ATTR:
      if (isSpace(c)) {
        return true;
      }
      if (c == '>') {
        return emitTag();
      }
      if (c == '/') {
        state_ = State::SELF_CLOSING;
        return true;
      }
      beginAttr();
      state_ = State::ATTR_NAME;
      [[fallthrough]];

    case State::ATTR_NAME:
      if (c == '=' || c == '>' || isSpace(c) || c == '/') {
        endAttrName();
        if (c == '=') {
          state_ = State::BEFORE_VALUE;
          return true;
        }
        if (c == '>') {
          return emitTag();
        }
        if (c == '/') {
          state_ = State::SELF_CLOSING;
          return true;
        }
        state_ = State::AFTER_ATTR_NAME;
        return true;
      }
      if (attrNameLen_ < ATTR_NAME_CAP - 1) {
        attrName_[attrNameLen_++] = toLowerAscii(c);
      } else {
        // Over-long names cannot match the allowlist; keep consuming so the
        // buffer never overflows and the value is discarded.
        attrNameLen_ = ATTR_NAME_CAP;
      }
      return true;

    case State::AFTER_ATTR_NAME:
      if (isSpace(c)) {
        return true;
      }
      if (c == '=') {
        state_ = State::BEFORE_VALUE;
        return true;
      }
      // A valueless attribute, immediately followed by the next one.
      state_ = State::BEFORE_ATTR;
      return consume(c);

    case State::BEFORE_VALUE:
      if (isSpace(c)) {
        return true;
      }
      if (c == '"' || c == '\'') {
        quoteChar_ = c;
        state_ = State::VALUE_QUOTED;
        return true;
      }
      if (c == '>') {
        finishAttrValue();
        return emitTag();
      }
      state_ = State::VALUE_UNQUOTED;
      putAttrValue(c);
      return true;

    case State::VALUE_QUOTED:
      // A '>' inside a quoted attribute value must not end the tag.
      if (c == quoteChar_) {
        finishAttrValue();
        state_ = State::BEFORE_ATTR;
        return true;
      }
      putAttrValue(c);
      return true;

    case State::VALUE_UNQUOTED:
      if (isSpace(c)) {
        finishAttrValue();
        state_ = State::BEFORE_ATTR;
        return true;
      }
      if (c == '>') {
        finishAttrValue();
        return emitTag();
      }
      putAttrValue(c);
      return true;

    case State::SELF_CLOSING:
      if (c == '>') {
        selfClosing_ = true;
        return emitTag();
      }
      // A stray '/' mid-tag: back to reading attributes.
      state_ = State::BEFORE_ATTR;
      return consume(c);

    case State::ENTITY:
      if (c == ';') {
        return decodeEntity();
      }
      if (entityLen_ >= ENTITY_CAP - 1 || c == '<' || c == '&' || isSpace(c)) {
        // Not an entity after all ("AT&T", a stray '&'). Emit literally and
        // reprocess the terminating character in TEXT state.
        if (!emitLiteral("&", 1) || !emitLiteral(entity_, entityLen_)) {
          return false;
        }
        state_ = State::TEXT;
        return consume(c);
      }
      entity_[entityLen_++] = c;
      return true;

    case State::MARKUP_OPEN:
      // Counting the opening dashes of "<!--". Two make it a comment; any
      // other character makes it a declaration (reprocessed there, since '>'
      // may already be that character: "<!>").
      if (c == '-') {
        if (++commentDashes_ == 2) {
          state_ = State::COMMENT;
          commentDashes_ = 0;
        }
        return true;
      }
      state_ = State::DECLARATION;
      return consume(c);

    case State::COMMENT:
      // A real comment ends only at "-->": a bare '>' inside it is content.
      if (c == '-') {
        if (commentDashes_ < 2) {
          ++commentDashes_;
        }
        return true;
      }
      if (c == '>' && commentDashes_ >= 2) {
        state_ = State::TEXT;
        return true;
      }
      commentDashes_ = 0;
      return true;

    case State::DECLARATION:
      if (c == '>') {
        state_ = State::TEXT;
      }
      return true;

    case State::RAWTEXT:
      if (c == '<') {
        state_ = State::RAWTEXT_MAYBE;
        rawMatchPos_ = 0;
      }
      return true;

    case State::RAWTEXT_MAYBE: {
      // Matching "</rawTag_". Position 0 expects '/', then the tag name.
      if (rawMatchPos_ == 0) {
        if (c == '/') {
          rawMatchPos_ = 1;
          return true;
        }
        state_ = State::RAWTEXT;
        return true;
      }
      const size_t nameIndex = rawMatchPos_ - 1;
      if (nameIndex < rawTagLen_) {
        if (toLowerAscii(c) == rawTag_[nameIndex]) {
          ++rawMatchPos_;
          return true;
        }
        state_ = State::RAWTEXT;
        return true;
      }
      // Full name matched; accept optional whitespace then '>'.
      if (c == '>') {
        state_ = State::TEXT;
        return handler_.onEndTag(rawTag_, rawTagLen_);
      }
      if (isSpace(c)) {
        return true;
      }
      state_ = State::RAWTEXT;
      return true;
    }
  }
  return true;
}

void HtmlTokenizer::beginAttr() {
  attrNameLen_ = 0;
  attrTarget_ = nullptr;
  attrTargetCap_ = 0;
  attrTargetLen_ = 0;
  attrTruncatedFlag_ = nullptr;
  attrOverflow_ = false;
}

void HtmlTokenizer::endAttrName() {
  if (capture_ == nullptr || attrNameLen_ >= ATTR_NAME_CAP) {
    return;
  }
  if (htmlEqualsIgnoreCase(attrName_, attrNameLen_, "src")) {
    attrTarget_ = capture_->src;
    attrTargetCap_ = HtmlAttrCapture::SRC_CAP;
    attrTruncatedFlag_ = &capture_->srcTruncated;
  } else if (htmlEqualsIgnoreCase(attrName_, attrNameLen_, "alt")) {
    attrTarget_ = capture_->alt;
    attrTargetCap_ = HtmlAttrCapture::ALT_CAP;
    attrTruncatedFlag_ = &capture_->altTruncated;
  } else if (htmlEqualsIgnoreCase(attrName_, attrNameLen_, "width")) {
    attrTarget_ = capture_->width;
    attrTargetCap_ = HtmlAttrCapture::NUM_CAP;
  } else if (htmlEqualsIgnoreCase(attrName_, attrNameLen_, "height")) {
    attrTarget_ = capture_->height;
    attrTargetCap_ = HtmlAttrCapture::NUM_CAP;
  } else if (htmlEqualsIgnoreCase(attrName_, attrNameLen_, "srcset")) {
    // Recorded as presence only. Picking a candidate out of a srcset would
    // mean parsing descriptors and guessing a viewport, and the plain src is
    // the better choice on a 1-bit panel anyway.
    capture_->hasSrcset = true;
  }
}

void HtmlTokenizer::putAttrValue(char c) {
  if (attrTarget_ == nullptr) {
    return;
  }
  if (attrTargetLen_ + 1 >= attrTargetCap_) {
    attrOverflow_ = true;
    return;
  }
  attrTarget_[attrTargetLen_++] = c;
}

void HtmlTokenizer::finishAttrValue() {
  if (attrTarget_ != nullptr) {
    if (attrOverflow_) {
      // Half a URL is worse than none: drop the value and say why.
      attrTarget_[0] = '\0';
      if (attrTruncatedFlag_ != nullptr) {
        *attrTruncatedFlag_ = true;
      }
    } else {
      attrTargetLen_ = decodeAttrEntities(attrTarget_, attrTargetLen_);
      attrTarget_[attrTargetLen_] = '\0';
    }
  }
  attrTarget_ = nullptr;
  attrTargetLen_ = 0;
  attrTruncatedFlag_ = nullptr;
  attrOverflow_ = false;
}

bool HtmlTokenizer::emitTag() {
  finishAttrValue();
  state_ = State::TEXT;

  if (tagNameLen_ == 0) {
    return true;
  }

  if (closingTag_) {
    return handler_.onEndTag(tagName_, tagNameLen_);
  }

  if (isRawTextTag(tagName_, tagNameLen_)) {
    memcpy(rawTag_, tagName_, tagNameLen_);
    rawTagLen_ = tagNameLen_;
    state_ = State::RAWTEXT;
    rawMatchPos_ = 0;
  }
  return handler_.onStartTag(tagName_, tagNameLen_, capture_, selfClosing_);
}

bool HtmlTokenizer::decodeEntity() {
  state_ = State::TEXT;

  if (entityLen_ == 0) {
    return emitLiteral("&;", 2);
  }

  uint32_t codepoint = 0;
  if (resolveEntity(entity_, entityLen_, &codepoint)) {
    return emitUtf8Literal(codepoint);
  }
  // Unrecognized or malformed: it was never a reference, so it is content.
  return emitLiteral("&", 1) && emitLiteral(entity_, entityLen_) && emitLiteral(";", 1);
}

bool HtmlTokenizer::putText(char c) {
  text_[textLen_++] = c;
  if (textLen_ == TEXT_CAP) {
    return flushText();
  }
  return true;
}

bool HtmlTokenizer::emitUtf8Literal(uint32_t codepoint) {
  char buf[4];
  return emitLiteral(buf, encodeUtf8(codepoint, buf));
}

// Flushes as a source-text run. Entity output must not go through here -- see
// emitLiteral and the `literal` flag on onText.
bool HtmlTokenizer::flushText() {
  if (textLen_ == 0) {
    return true;
  }
  const size_t len = textLen_;
  textLen_ = 0;
  if (!handler_.onText(text_, len, false)) {
    aborted_ = true;
    return false;
  }
  return true;
}

bool HtmlTokenizer::emitLiteral(const char* data, size_t len) {
  if (!flushText()) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  if (!handler_.onText(data, len, true)) {
    aborted_ = true;
    return false;
  }
  return true;
}

}  // namespace readwise
