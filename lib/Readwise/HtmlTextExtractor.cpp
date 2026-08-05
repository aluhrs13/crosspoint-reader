#include "HtmlTextExtractor.h"

#include <cstring>

namespace readwise {
namespace {

bool equalsIgnoreCase(const char* a, size_t aLen, const char* b) {
  const size_t bLen = strlen(b);
  if (aLen != bLen) {
    return false;
  }
  for (size_t i = 0; i < aLen; ++i) {
    char ca = a[i];
    if (ca >= 'A' && ca <= 'Z') {
      ca = static_cast<char>(ca - 'A' + 'a');
    }
    if (ca != b[i]) {
      return false;
    }
  }
  return true;
}

// Elements that terminate a paragraph-level run. Both the opening and closing
// tag produce a break, which collapses to at most one blank line.
bool isBlockTag(const char* name, size_t len) {
  static const char* const kBlocks[] = {"p",          "div",     "h1",         "h2",     "h3",      "h4",
                                        "h5",         "h6",      "blockquote", "ul",     "ol",      "table",
                                        "tr",         "section", "article",    "header", "footer",  "figure",
                                        "figcaption", "pre",     "hr",         "aside",  "details", "summary"};
  for (const char* block : kBlocks) {
    if (equalsIgnoreCase(name, len, block)) {
      return true;
    }
  }
  return false;
}

bool isRawTextTag(const char* name, size_t len) {
  return equalsIgnoreCase(name, len, "script") || equalsIgnoreCase(name, len, "style") ||
         equalsIgnoreCase(name, len, "template");
}

char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

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

}  // namespace

HtmlTextExtractor::HtmlTextExtractor(TextSink sink, void* ctx) : sink_(sink), ctx_(ctx) { reset(); }

void HtmlTextExtractor::reset() {
  state_ = State::TEXT;
  tagNameLen_ = 0;
  closingTag_ = false;
  quoteChar_ = 0;
  entityLen_ = 0;
  rawTagLen_ = 0;
  rawMatchPos_ = 0;
  commentDashes_ = 0;
  pendingNewlines_ = 0;
  pendingSpace_ = false;
  emittedAny_ = false;
  aborted_ = false;
  outLen_ = 0;
}

bool HtmlTextExtractor::feed(const char* data, size_t len) {
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

bool HtmlTextExtractor::finish() {
  if (aborted_) {
    return false;
  }
  // An unterminated entity at end of input is emitted literally.
  if (state_ == State::ENTITY) {
    if (!emitPendingBreaks() || !put('&')) {
      return false;
    }
    for (size_t i = 0; i < entityLen_; ++i) {
      if (!put(entity_[i])) {
        return false;
      }
    }
  }
  if (emittedAny_) {
    out_[outLen_ < OUT_CAP ? outLen_++ : outLen_ - 1] = '\n';
  }
  return flushOutput();
}

bool HtmlTextExtractor::consume(char c) {
  switch (state_) {
    case State::TEXT:
      if (c == '<') {
        state_ = State::TAG_OPEN;
        tagNameLen_ = 0;
        closingTag_ = false;
        return true;
      }
      if (c == '&') {
        state_ = State::ENTITY;
        entityLen_ = 0;
        return true;
      }
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        // Whitespace in HTML source collapses; it becomes one space unless a
        // block break is already pending.
        if (emittedAny_ && pendingNewlines_ == 0) {
          pendingSpace_ = true;
        }
        return true;
      }
      if (!emitPendingBreaks()) {
        return false;
      }
      return put(c);

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
        return handleTagComplete();
      }
      state_ = State::TAG_REST;
      return true;

    case State::TAG_REST:
      if (c == '"' || c == '\'') {
        quoteChar_ = c;
        state_ = State::TAG_QUOTE;
        return true;
      }
      if (c == '>') {
        return handleTagComplete();
      }
      return true;

    case State::TAG_QUOTE:
      // A '>' inside a quoted attribute value must not end the tag.
      if (c == quoteChar_) {
        state_ = State::TAG_REST;
      }
      return true;

    case State::ENTITY:
      if (c == ';') {
        return decodeEntity();
      }
      if (entityLen_ >= ENTITY_CAP - 1 || c == '<' || c == '&' || c == ' ') {
        // Not an entity after all ("AT&T", a stray '&'). Emit literally and
        // reprocess the terminating character in TEXT state.
        if (!emitPendingBreaks() || !put('&')) {
          return false;
        }
        for (size_t i = 0; i < entityLen_; ++i) {
          if (!put(entity_[i])) {
            return false;
          }
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
        return true;
      }
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        return true;
      }
      state_ = State::RAWTEXT;
      return true;
    }
  }
  return true;
}

bool HtmlTextExtractor::handleTagComplete() {
  state_ = State::TEXT;

  if (!closingTag_ && isRawTextTag(tagName_, tagNameLen_)) {
    memcpy(rawTag_, tagName_, tagNameLen_);
    rawTagLen_ = tagNameLen_;
    state_ = State::RAWTEXT;
    return true;
  }

  if (equalsIgnoreCase(tagName_, tagNameLen_, "br")) {
    if (emittedAny_ && pendingNewlines_ < 1) {
      pendingNewlines_ = 1;
    }
    pendingSpace_ = false;
    return true;
  }

  if (equalsIgnoreCase(tagName_, tagNameLen_, "li")) {
    if (!closingTag_) {
      if (emittedAny_ && pendingNewlines_ < 1) {
        pendingNewlines_ = 1;
      }
      pendingSpace_ = false;
      if (!emitPendingBreaks()) {
        return false;
      }
      // A bullet keeps list items readable as plain text.
      return put('-') && put(' ');
    }
    if (emittedAny_ && pendingNewlines_ < 1) {
      pendingNewlines_ = 1;
    }
    pendingSpace_ = false;
    return true;
  }

  if (isBlockTag(tagName_, tagNameLen_)) {
    // Blocks separate with one blank line; consecutive block tags collapse.
    if (emittedAny_) {
      pendingNewlines_ = 2;
    }
    pendingSpace_ = false;
  }
  return true;
}

bool HtmlTextExtractor::decodeEntity() {
  state_ = State::TEXT;

  if (entityLen_ == 0) {
    return put('&') && put(';');
  }

  if (entity_[0] == '#') {
    uint32_t codepoint = 0;
    bool valid = entityLen_ > 1;
    if (entityLen_ > 2 && (entity_[1] == 'x' || entity_[1] == 'X')) {
      for (size_t i = 2; i < entityLen_ && valid; ++i) {
        const char c = entity_[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
          digit = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
          digit = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
          digit = static_cast<uint32_t>(c - 'A' + 10);
        } else {
          valid = false;
          break;
        }
        codepoint = codepoint * 16 + digit;
      }
    } else {
      for (size_t i = 1; i < entityLen_ && valid; ++i) {
        const char c = entity_[i];
        if (c < '0' || c > '9') {
          valid = false;
          break;
        }
        codepoint = codepoint * 10 + static_cast<uint32_t>(c - '0');
      }
    }
    if (valid && codepoint > 0 && codepoint <= 0x10FFFF) {
      if (!emitPendingBreaks()) {
        return false;
      }
      return putUtf8(codepoint);
    }
    // Malformed numeric reference: emit literally.
  } else {
    for (const NamedEntity& entry : kEntities) {
      if (equalsIgnoreCase(entity_, entityLen_, entry.name)) {
        if (!emitPendingBreaks()) {
          return false;
        }
        if (entry.codepoint == 0x00A0) {
          // Non-breaking space renders as an ordinary space in plain text.
          return put(' ');
        }
        return putUtf8(entry.codepoint);
      }
    }
  }

  if (!emitPendingBreaks() || !put('&')) {
    return false;
  }
  for (size_t i = 0; i < entityLen_; ++i) {
    if (!put(entity_[i])) {
      return false;
    }
  }
  return put(';');
}

bool HtmlTextExtractor::emitPendingBreaks() {
  if (pendingNewlines_ > 0) {
    for (uint8_t i = 0; i < pendingNewlines_; ++i) {
      if (!put('\n')) {
        return false;
      }
    }
    pendingNewlines_ = 0;
    pendingSpace_ = false;
    return true;
  }
  if (pendingSpace_) {
    pendingSpace_ = false;
    return put(' ');
  }
  return true;
}

bool HtmlTextExtractor::put(char c) {
  emittedAny_ = true;
  out_[outLen_++] = c;
  if (outLen_ == OUT_CAP) {
    return flushOutput();
  }
  return true;
}

bool HtmlTextExtractor::putUtf8(uint32_t codepoint) {
  emittedAny_ = true;
  if (codepoint < 0x80) {
    return put(static_cast<char>(codepoint));
  }
  if (codepoint < 0x800) {
    return put(static_cast<char>(0xC0 | (codepoint >> 6))) && put(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  if (codepoint < 0x10000) {
    return put(static_cast<char>(0xE0 | (codepoint >> 12))) &&
           put(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F))) &&
           put(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return put(static_cast<char>(0xF0 | (codepoint >> 18))) &&
         put(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F))) &&
         put(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F))) && put(static_cast<char>(0x80 | (codepoint & 0x3F)));
}

bool HtmlTextExtractor::flushOutput() {
  if (outLen_ == 0) {
    return true;
  }
  const bool ok = sink_ == nullptr || sink_(ctx_, out_, outLen_);
  outLen_ = 0;
  if (!ok) {
    aborted_ = true;
  }
  return ok;
}

}  // namespace readwise
