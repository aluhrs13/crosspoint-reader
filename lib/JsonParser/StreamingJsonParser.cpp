#include "StreamingJsonParser.h"

#include <cstring>

StreamingJsonParser::StreamingJsonParser(const JsonCallbacks& callbacks) : cb(callbacks) { reset(); }

void StreamingJsonParser::reset() {
  tokenLen = 0;
  state = State::SCANNING;
  expectingValue = false;
  escaped = false;
  tokenOverflow = false;
  error = false;
  nestingDepth = 0;
  literalLen = 0;
  literalPos = 0;
  unicodeDigits = 0;
  unicodeValue = 0;
  pendingSurrogate = 0;
}

void StreamingJsonParser::feed(const char* data, size_t len) {
  for (size_t i = 0; i < len && !error; ++i) {
    char c = data[i];
    switch (state) {
      case State::SCANNING:
        handleScanning(c);
        break;
      case State::IN_STRING_KEY:
      case State::IN_STRING_VALUE:
        handleStringChar(c);
        break;
      case State::IN_NUMBER:
        handleNumber(c);
        break;
      case State::IN_LITERAL:
        handleLiteral(c);
        break;
      case State::SKIP_STRING:
        handleSkipString(c);
        break;
    }
  }
}

void StreamingJsonParser::handleScanning(char c) {
  switch (c) {
    case '"':
      tokenLen = 0;
      tokenOverflow = false;
      if (expectingValue || inArray()) {
        state = State::IN_STRING_VALUE;
      } else {
        state = State::IN_STRING_KEY;
      }
      break;
    case '{':
      if (nestingDepth < MAX_NESTING) {
        nestingStack[nestingDepth++] = Container::OBJECT;
      } else {
        error = true;
        return;
      }
      if (cb.onObjectStart) cb.onObjectStart(cb.ctx);
      expectingValue = false;
      break;
    case '}':
      if (cb.onObjectEnd) cb.onObjectEnd(cb.ctx);
      if (nestingDepth > 0) --nestingDepth;
      expectingValue = false;
      break;
    case '[':
      if (nestingDepth < MAX_NESTING) {
        nestingStack[nestingDepth++] = Container::ARRAY;
      } else {
        error = true;
        return;
      }
      if (cb.onArrayStart) cb.onArrayStart(cb.ctx);
      expectingValue = false;
      break;
    case ']':
      if (cb.onArrayEnd) cb.onArrayEnd(cb.ctx);
      if (nestingDepth > 0) --nestingDepth;
      expectingValue = false;
      break;
    case ':':
      expectingValue = true;
      break;
    case ',':
      expectingValue = false;
      break;
    case 't':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "true", 4);
        literalLen = 4;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    case 'f':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "false", 5);
        literalLen = 5;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    case 'n':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "null", 4);
        literalLen = 4;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    default:
      if ((expectingValue || inArray()) && (c == '-' || (c >= '0' && c <= '9'))) {
        tokenLen = 0;
        tokenOverflow = false;
        appendToken(c);
        state = State::IN_NUMBER;
      }
      break;
  }
}

void StreamingJsonParser::handleStringChar(char c) {
  if (unicodeDigits > 0) {
    handleUnicodeHexDigit(c);
    return;
  }

  if (escaped) {
    escaped = false;
    // Any decoded escape other than the second half of a surrogate pair means
    // a pending high surrogate was unpaired.
    if (c != 'u') {
      flushPendingSurrogate();
    }
    switch (c) {
      case '"':
      case '\\':
      case '/':
        appendToken(c);
        break;
      case 'b':
        appendToken('\b');
        break;
      case 'f':
        appendToken('\f');
        break;
      case 'n':
        appendToken('\n');
        break;
      case 'r':
        appendToken('\r');
        break;
      case 't':
        appendToken('\t');
        break;
      case 'u':
        // Begin collecting the four hex digits. Decoded incrementally so a
        // chunk boundary can land anywhere inside the escape.
        unicodeDigits = 1;
        unicodeValue = 0;
        break;
      default:
        appendToken('\\');
        appendToken(c);
        break;
    }
    return;
  }

  if (c == '\\') {
    escaped = true;
    return;
  }

  flushPendingSurrogate();

  if (c == '"') {
    emitToken();
    return;
  }

  appendToken(c);
}

void StreamingJsonParser::handleUnicodeHexDigit(char c) {
  uint32_t digit;
  if (c >= '0' && c <= '9') {
    digit = static_cast<uint32_t>(c - '0');
  } else if (c >= 'a' && c <= 'f') {
    digit = static_cast<uint32_t>(c - 'a' + 10);
  } else if (c >= 'A' && c <= 'F') {
    digit = static_cast<uint32_t>(c - 'A' + 10);
  } else {
    // Malformed escape: JSON guarantees four hex digits.
    error = true;
    return;
  }
  unicodeValue = (unicodeValue << 4) | digit;
  if (++unicodeDigits <= 4) {
    return;
  }
  unicodeDigits = 0;

  const uint32_t value = unicodeValue;
  if (value >= 0xD800 && value <= 0xDBFF) {
    // High surrogate: hold it for the immediately-following low half. If one
    // is already pending, it was unpaired.
    flushPendingSurrogate();
    pendingSurrogate = value;
    return;
  }
  if (value >= 0xDC00 && value <= 0xDFFF) {
    if (pendingSurrogate != 0) {
      const uint32_t codepoint = 0x10000 + ((pendingSurrogate - 0xD800) << 10) + (value - 0xDC00);
      pendingSurrogate = 0;
      emitCodepoint(codepoint);
    } else {
      // Lone low surrogate.
      emitCodepoint(0xFFFD);
    }
    return;
  }
  flushPendingSurrogate();
  emitCodepoint(value);
}

void StreamingJsonParser::flushPendingSurrogate() {
  if (pendingSurrogate != 0) {
    pendingSurrogate = 0;
    emitCodepoint(0xFFFD);
  }
}

void StreamingJsonParser::emitCodepoint(uint32_t codepoint) {
  if (codepoint < 0x80) {
    appendToken(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    appendToken(static_cast<char>(0xC0 | (codepoint >> 6)));
    appendToken(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint < 0x10000) {
    appendToken(static_cast<char>(0xE0 | (codepoint >> 12)));
    appendToken(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    appendToken(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    appendToken(static_cast<char>(0xF0 | (codepoint >> 18)));
    appendToken(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    appendToken(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    appendToken(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

void StreamingJsonParser::handleNumber(char c) {
  if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
    appendToken(c);
    return;
  }

  if (!tokenOverflow && cb.onNumber) {
    tokenBuf[tokenLen] = '\0';
    cb.onNumber(cb.ctx, tokenBuf, tokenLen);
  }
  state = State::SCANNING;
  expectingValue = false;

  handleScanning(c);
}

void StreamingJsonParser::handleLiteral(char c) {
  if (c == literalExpected[literalPos]) {
    ++literalPos;
    if (literalPos == literalLen) {
      if (literalExpected[0] == 't') {
        if (cb.onBool) cb.onBool(cb.ctx, true);
      } else if (literalExpected[0] == 'f') {
        if (cb.onBool) cb.onBool(cb.ctx, false);
      } else {
        if (cb.onNull) cb.onNull(cb.ctx);
      }
      state = State::SCANNING;
      expectingValue = false;
    }
  } else {
    error = true;
  }
}

void StreamingJsonParser::handleSkipString(char c) {
  if (escaped) {
    escaped = false;
    return;
  }
  if (c == '\\') {
    escaped = true;
    return;
  }
  if (c == '"') {
    state = State::SCANNING;
    expectingValue = false;
  }
}

void StreamingJsonParser::appendToken(char c) {
  if (tokenLen < TOKEN_BUF_SIZE - 1) {
    tokenBuf[tokenLen++] = c;
    return;
  }
  // With a streaming sink installed, a full buffer during a string VALUE is
  // flushed as a chunk and parsing continues -- values of any length pass
  // through. Everything else (keys, numbers, or values without a sink) keeps
  // the legacy behaviour: mark overflow and drop the token at emit time.
  if (state == State::IN_STRING_VALUE && streamingValues()) {
    flushValueChunk();
    tokenBuf[tokenLen++] = c;
    return;
  }
  tokenOverflow = true;
}

void StreamingJsonParser::flushValueChunk() {
  if (tokenLen > 0) {
    cb.onStringChunk(cb.ctx, tokenBuf, tokenLen);
    tokenLen = 0;
  }
}

void StreamingJsonParser::emitToken() {
  if (state == State::IN_STRING_KEY) {
    if (!tokenOverflow && cb.onKey) {
      tokenBuf[tokenLen] = '\0';
      cb.onKey(cb.ctx, tokenBuf, tokenLen);
    }
    state = State::SCANNING;
  } else {
    if (streamingValues()) {
      // Chunked delivery: remaining bytes, then the end marker. An empty string
      // is zero chunks followed by onStringEnd.
      flushValueChunk();
      cb.onStringEnd(cb.ctx);
    } else if (!tokenOverflow && cb.onString) {
      tokenBuf[tokenLen] = '\0';
      cb.onString(cb.ctx, tokenBuf, tokenLen);
    }
    state = State::SCANNING;
    expectingValue = false;
  }
}
