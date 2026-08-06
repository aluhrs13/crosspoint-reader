#include "HtmlTextExtractor.h"

#include <cstring>

namespace readwise {
namespace {

// Elements that terminate a paragraph-level run. Both the opening and closing
// tag produce a break, which collapses to at most one blank line.
bool isBlockTag(const char* name, size_t len) {
  static const char* const kBlocks[] = {"p",          "div",     "h1",         "h2",     "h3",      "h4",
                                        "h5",         "h6",      "blockquote", "ul",     "ol",      "table",
                                        "tr",         "section", "article",    "header", "footer",  "figure",
                                        "figcaption", "pre",     "hr",         "aside",  "details", "summary"};
  for (const char* block : kBlocks) {
    if (htmlEqualsIgnoreCase(name, len, block)) {
      return true;
    }
  }
  return false;
}

bool isSourceSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace

HtmlTextExtractor::HtmlTextExtractor(TextSink sink, void* ctx) : sink_(sink), ctx_(ctx), tokenizer_(*this) { reset(); }

void HtmlTextExtractor::reset() {
  tokenizer_.reset();
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
  if (!tokenizer_.feed(data, len)) {
    aborted_ = true;
    return false;
  }
  return true;
}

bool HtmlTextExtractor::finish() {
  if (aborted_ || !tokenizer_.finish()) {
    return false;
  }
  if (emittedAny_) {
    out_[outLen_ < OUT_CAP ? outLen_++ : outLen_ - 1] = '\n';
  }
  return flushOutput();
}

bool HtmlTextExtractor::onText(const char* data, size_t len, bool literal) {
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (!literal && isSourceSpace(c)) {
      // Whitespace in HTML source collapses; it becomes one space unless a
      // block break is already pending.
      if (emittedAny_ && pendingNewlines_ == 0) {
        pendingSpace_ = true;
      }
      continue;
    }
    // U+00A0 arrives here only from &nbsp; or a numeric reference, so it is
    // deliberate content -- but plain text has no non-breaking space, and
    // leaving the raw bytes in would render as a stray glyph.
    if (literal && i + 1 < len && static_cast<unsigned char>(c) == 0xC2 &&
        static_cast<unsigned char>(data[i + 1]) == 0xA0) {
      if (!emitPendingBreaks() || !put(' ')) {
        return false;
      }
      ++i;
      continue;
    }
    if (!emitPendingBreaks() || !put(c)) {
      return false;
    }
  }
  return true;
}

bool HtmlTextExtractor::onStartTag(const char* name, size_t len, const HtmlAttrCapture*, bool) {
  return handleTag(name, len, false);
}

bool HtmlTextExtractor::onEndTag(const char* name, size_t len) { return handleTag(name, len, true); }

bool HtmlTextExtractor::handleTag(const char* name, size_t len, bool closing) {
  if (htmlEqualsIgnoreCase(name, len, "br")) {
    if (emittedAny_ && pendingNewlines_ < 1) {
      pendingNewlines_ = 1;
    }
    pendingSpace_ = false;
    return true;
  }

  if (htmlEqualsIgnoreCase(name, len, "li")) {
    if (!closing) {
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

  if (isBlockTag(name, len)) {
    // Blocks separate with one blank line; consecutive block tags collapse.
    if (emittedAny_) {
      pendingNewlines_ = 2;
    }
    pendingSpace_ = false;
  }
  return true;
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
