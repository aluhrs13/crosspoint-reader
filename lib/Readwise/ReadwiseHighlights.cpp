#include "ReadwiseHighlights.h"

#include <cstring>

namespace readwise {
namespace {

// Byte-wise little-endian accessors, duplicated from ReadwiseCodec.cpp's
// anonymous namespace: endian-independent and never an unaligned load.
void putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint16_t getU16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (static_cast<uint16_t>(in[1]) << 8)); }

void putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool isTerminator(const char* buf, size_t len, size_t pos, size_t* termLen) {
  const char c = buf[pos];
  if (c == '.' || c == '!' || c == '?') {
    *termLen = 1;
    return true;
  }
  // U+2026 HORIZONTAL ELLIPSIS. Continuation bytes never match ASCII values,
  // so matching this exact sequence is UTF-8 safe.
  if (static_cast<uint8_t>(c) == 0xE2 && pos + 2 < len && static_cast<uint8_t>(buf[pos + 1]) == 0x80 &&
      static_cast<uint8_t>(buf[pos + 2]) == 0xA6) {
    *termLen = 3;
    return true;
  }
  return false;
}

bool isClosingMark(const char* buf, size_t len, size_t pos, size_t* markLen) {
  const char c = buf[pos];
  if (c == '"' || c == '\'' || c == ')' || c == ']') {
    *markLen = 1;
    return true;
  }
  // U+201D and U+2019, the curly closing quotes (E2 80 9D / E2 80 99).
  if (static_cast<uint8_t>(c) == 0xE2 && pos + 2 < len && static_cast<uint8_t>(buf[pos + 1]) == 0x80 &&
      (static_cast<uint8_t>(buf[pos + 2]) == 0x9D || static_cast<uint8_t>(buf[pos + 2]) == 0x99)) {
    *markLen = 3;
    return true;
  }
  return false;
}

}  // namespace

// --- highlight record codec -----------------------------------------------

size_t encodeHighlight(const HighlightRecord& rec, uint8_t* out, size_t outCap) {
  if (out == nullptr || rec.textLen > HIGHLIGHT_TEXT_CAP) {
    return 0;
  }
  const size_t payloadLen = HIGHLIGHT_FIXED_PAYLOAD + rec.textLen;
  const size_t total = 2 + payloadLen;
  if (outCap < total) {
    return 0;
  }
  putU16(out, static_cast<uint16_t>(payloadLen));
  out[HIGHLIGHT_FLAGS_OFFSET] = rec.flags;
  putU32(out + 3, rec.startByte);
  putU32(out + 7, rec.endByte);
  putU32(out + 11, rec.bodySize);
  putU16(out + 15, rec.textLen);
  memcpy(out + 17, rec.text, rec.textLen);
  return total;
}

bool decodeHighlight(const uint8_t* in, size_t len, HighlightRecord& rec, size_t* consumed) {
  if (in == nullptr || len < 2) {
    return false;
  }
  const uint16_t payloadLen = getU16(in);
  if (payloadLen < HIGHLIGHT_FIXED_PAYLOAD || 2 + static_cast<size_t>(payloadLen) > len) {
    return false;
  }
  rec = HighlightRecord();
  rec.flags = in[HIGHLIGHT_FLAGS_OFFSET];
  rec.startByte = getU32(in + 3);
  rec.endByte = getU32(in + 7);
  rec.bodySize = getU32(in + 11);
  rec.textLen = getU16(in + 15);
  // The two lengths are redundant; a mismatch means corruption, and textLen
  // must never overrun the destination buffer.
  if (rec.textLen > HIGHLIGHT_TEXT_CAP || static_cast<size_t>(rec.textLen) != payloadLen - HIGHLIGHT_FIXED_PAYLOAD) {
    return false;
  }
  memcpy(rec.text, in + 17, rec.textLen);
  rec.text[rec.textLen] = '\0';
  if (consumed != nullptr) {
    *consumed = 2 + static_cast<size_t>(payloadLen);
  }
  return true;
}

// --- sentence scanner ------------------------------------------------------

void scanSentences(const char* buf, size_t len, uint32_t baseOffset, std::vector<SentenceSpan>& out, size_t maxSpans) {
  if (buf == nullptr || maxSpans == 0) {
    return;
  }
  size_t pos = 0;
  while (pos < len && out.size() < maxSpans) {
    while (pos < len && isAsciiSpace(buf[pos])) {
      pos++;
    }
    if (pos >= len) {
      return;
    }
    const size_t spanStart = pos;
    size_t spanEnd = 0;
    while (pos < len) {
      if (buf[pos] == '\n') {
        // Newlines only separate blocks in extracted article text, so a
        // heading or list item without a terminator still forms a span.
        spanEnd = pos;
        break;
      }
      size_t termLen = 0;
      if (isTerminator(buf, len, pos, &termLen)) {
        // Consume the full terminator run ("...", "?!") and any closing
        // quotes/brackets so they stay inside the sentence.
        size_t after = pos + termLen;
        size_t moreLen = 0;
        while (after < len && (isTerminator(buf, len, after, &moreLen) || isClosingMark(buf, len, after, &moreLen))) {
          after += moreLen;
        }
        if (after >= len || isAsciiSpace(buf[after])) {
          spanEnd = after;
          pos = after;
          break;
        }
        // Terminator mid-token (e.g. "3.14", "e.g.x") -- keep scanning.
        pos = after;
        continue;
      }
      pos++;
    }
    if (spanEnd == 0) {
      // Ran off the end of the buffer without a terminator: the tail is still
      // a selectable span, trimmed of trailing whitespace.
      spanEnd = len;
      while (spanEnd > spanStart && isAsciiSpace(buf[spanEnd - 1])) {
        spanEnd--;
      }
      pos = len;
    }
    if (spanEnd > spanStart) {
      out.push_back({baseOffset + static_cast<uint32_t>(spanStart), baseOffset + static_cast<uint32_t>(spanEnd)});
    }
  }
}

}  // namespace readwise
