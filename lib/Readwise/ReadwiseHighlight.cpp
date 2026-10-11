#include "ReadwiseHighlight.h"

#include <cstdio>
#include <cstring>

namespace readwise {
namespace {

bool appendRaw(char* out, size_t cap, size_t& pos, const char* bytes, size_t len) {
  if (pos + len >= cap) {
    return false;
  }
  memcpy(out + pos, bytes, len);
  pos += len;
  return true;
}

bool appendJsonString(char* out, size_t cap, size_t& pos, const char* value) {
  if (!appendRaw(out, cap, pos, "\"", 1)) {
    return false;
  }
  for (const auto* p = reinterpret_cast<const uint8_t*>(value); *p != 0; ++p) {
    if (*p == '"' || *p == '\\') {
      const char escaped[2] = {'\\', static_cast<char>(*p)};
      if (!appendRaw(out, cap, pos, escaped, 2)) {
        return false;
      }
    } else if (*p == '\n') {
      if (!appendRaw(out, cap, pos, "\\n", 2)) {
        return false;
      }
    } else if (*p == '\r') {
      if (!appendRaw(out, cap, pos, "\\r", 2)) {
        return false;
      }
    } else if (*p == '\t') {
      if (!appendRaw(out, cap, pos, "\\t", 2)) {
        return false;
      }
    } else if (*p < 0x20) {
      char hex[7];
      snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned>(*p));
      if (!appendRaw(out, cap, pos, hex, 6)) {
        return false;
      }
    } else if (!appendRaw(out, cap, pos, reinterpret_cast<const char*>(p), 1)) {
      return false;
    }
  }
  return appendRaw(out, cap, pos, "\"", 1);
}

bool appendField(char* out, size_t cap, size_t& pos, const char* key, const char* value, bool& first) {
  if (value == nullptr || value[0] == '\0') {
    return true;
  }
  if (!first && !appendRaw(out, cap, pos, ",", 1)) {
    return false;
  }
  first = false;
  return appendRaw(out, cap, pos, key, strlen(key)) && appendJsonString(out, cap, pos, value);
}

}  // namespace

bool fitHighlightText(char* text, size_t cap) {
  if (text == nullptr || cap < 2) {
    return false;
  }
  const size_t limit = cap - 1 < HIGHLIGHT_TEXT_MAX ? cap - 1 : HIGHLIGHT_TEXT_MAX;
  size_t len = 0;
  while (text[len] != '\0') {
    ++len;
  }
  if (len > limit) {
    size_t cut = limit;
    while (cut > 0 && (static_cast<uint8_t>(text[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    size_t word = cut;
    while (word > 0 && text[word - 1] != ' ') {
      --word;
    }
    if (word > 0) {
      cut = word - 1;
    }
    text[cut] = '\0';
  }
  return text[0] != '\0';
}

bool buildHighlightBody(const char* text, const char* title, const char* author, const char* sourceUrl, char* out,
                        size_t outCap) {
  if (out == nullptr || outCap == 0 || text == nullptr || text[0] == '\0') {
    return false;
  }
  size_t pos = 0;
  if (!appendRaw(out, outCap, pos, "{\"highlights\":[{", 16)) {
    return false;
  }
  bool first = true;
  if (!appendField(out, outCap, pos, "\"text\":", text, first) ||
      !appendField(out, outCap, pos, "\"title\":", title, first) ||
      !appendField(out, outCap, pos, "\"author\":", author, first) ||
      !appendField(out, outCap, pos, "\"source_url\":", sourceUrl, first)) {
    return false;
  }
  if (!first && !appendRaw(out, outCap, pos, ",", 1)) {
    return false;
  }
  if (!appendRaw(out, outCap, pos, "\"source_type\":\"", 15) ||
      !appendRaw(out, outCap, pos, HIGHLIGHT_SOURCE_TYPE, strlen(HIGHLIGHT_SOURCE_TYPE)) ||
      !appendRaw(out, outCap, pos, "\",\"category\":\"articles\"}]}", 26)) {
    return false;
  }
  if (pos >= outCap) {
    return false;
  }
  out[pos] = '\0';
  return true;
}

namespace {

bool nextToken(const char*& p, const char*& token, uint16_t& length) {
  while (*p == ' ') {
    ++p;
  }
  if (*p == '\0') {
    return false;
  }
  token = p;
  while (*p != '\0' && *p != ' ') {
    ++p;
  }
  length = static_cast<uint16_t>(p - token);
  return length > 0;
}

uint16_t countTokens(const char* quote) {
  uint16_t count = 0;
  const char* token = nullptr;
  uint16_t length = 0;
  const char* p = quote != nullptr ? quote : "";
  while (nextToken(p, token, length)) {
    ++count;
  }
  return count;
}

bool sameWord(const HighlightWordView& word, const char* token, uint16_t length) {
  return word.text != nullptr && word.length == length && memcmp(word.text, token, length) == 0;
}

// True when `count` words at `start` equal quote tokens [first, first + count).
bool tokensEqual(const HighlightWordView* words, uint16_t start, const char* quote, uint16_t first, uint16_t count) {
  const char* p = quote != nullptr ? quote : "";
  uint16_t index = 0;
  const char* token = nullptr;
  uint16_t length = 0;
  while (nextToken(p, token, length)) {
    if (index >= first) {
      const uint16_t offset = static_cast<uint16_t>(index - first);
      if (offset >= count || !sameWord(words[start + offset], token, length)) {
        return false;
      }
    }
    ++index;
    if (index >= first + count) {
      return true;
    }
  }
  return false;
}

uint16_t matchLengthAt(const HighlightWordView* words, uint16_t wordCount, uint16_t start, const char* quote) {
  const char* p = quote != nullptr ? quote : "";
  uint16_t matched = 0;
  const char* token = nullptr;
  uint16_t length = 0;
  while (nextToken(p, token, length)) {
    if (start + matched >= wordCount || !sameWord(words[start + matched], token, length)) {
      return 0;
    }
    ++matched;
  }
  return matched;
}

void paint(uint8_t* marks, uint16_t start, uint16_t count, bool posted) {
  const uint8_t style = static_cast<uint8_t>(posted ? HighlightMark::Posted : HighlightMark::Pending);
  for (uint16_t i = 0; i < count; ++i) {
    if (marks[start + i] == static_cast<uint8_t>(HighlightMark::Posted)) {
      continue;
    }
    if (posted || marks[start + i] == static_cast<uint8_t>(HighlightMark::None)) {
      marks[start + i] = style;
    }
  }
}

}  // namespace

void markHighlightWords(const HighlightWordView* words, const uint16_t wordCount, const char* const* quotes,
                        const uint8_t* quoteFlags, const uint16_t quoteCount, uint8_t* marks) {
  if (marks == nullptr) {
    return;
  }
  if (wordCount > 0) {
    memset(marks, 0, wordCount);
  }
  if (words == nullptr || quotes == nullptr || quoteFlags == nullptr || wordCount == 0) {
    return;
  }
  for (uint16_t q = 0; q < quoteCount; ++q) {
    const char* quote = quotes[q];
    if (quote == nullptr || quote[0] == '\0') {
      continue;
    }
    const bool posted = (quoteFlags[q] & HIGHLIGHT_FLAG_POSTED) != 0;
    const uint16_t tokens = countTokens(quote);
    if (tokens == 0) {
      continue;
    }
    bool full = false;
    for (uint16_t i = 0; i < wordCount;) {
      const uint16_t matched = matchLengthAt(words, wordCount, i, quote);
      if (matched > 0) {
        paint(marks, i, matched, posted);
        full = true;
        i = static_cast<uint16_t>(i + matched);
      } else {
        ++i;
      }
    }
    if (full || tokens < HIGHLIGHT_PARTIAL_MIN_WORDS || wordCount < HIGHLIGHT_PARTIAL_MIN_WORDS) {
      continue;
    }
    const uint16_t limit = wordCount < tokens ? wordCount : static_cast<uint16_t>(tokens - 1);
    for (uint16_t k = limit; k >= HIGHLIGHT_PARTIAL_MIN_WORDS; --k) {
      if (tokensEqual(words, static_cast<uint16_t>(wordCount - k), quote, 0, k)) {
        paint(marks, static_cast<uint16_t>(wordCount - k), k, posted);
        break;
      }
    }
    for (uint16_t k = limit; k >= HIGHLIGHT_PARTIAL_MIN_WORDS; --k) {
      if (tokensEqual(words, 0, quote, static_cast<uint16_t>(tokens - k), k)) {
        paint(marks, 0, k, posted);
        break;
      }
    }
  }
}

}  // namespace readwise
