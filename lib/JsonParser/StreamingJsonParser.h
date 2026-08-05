#pragma once

#include <cstddef>
#include <cstdint>

struct JsonCallbacks {
  void* ctx;
  void (*onKey)(void* ctx, const char* key, size_t len);
  void (*onString)(void* ctx, const char* value, size_t len);
  void (*onNumber)(void* ctx, const char* value, size_t len);
  void (*onBool)(void* ctx, bool value);
  void (*onNull)(void* ctx);
  void (*onObjectStart)(void* ctx);
  void (*onObjectEnd)(void* ctx);
  void (*onArrayStart)(void* ctx);
  void (*onArrayEnd)(void* ctx);

  // Optional streaming sink for string VALUES (never keys). When BOTH are set,
  // every string value is delivered as zero or more onStringChunk calls holding
  // decoded bytes, followed by exactly one onStringEnd -- and onString is not
  // called for values at all. Values of any length then stream through the
  // fixed token buffer instead of being dropped on overflow, which is what the
  // legacy behaviour does (see appendToken).
  //
  // Chunks split at arbitrary byte positions, including inside multi-byte UTF-8
  // sequences; consumers writing to a file can concatenate blindly, consumers
  // interpreting text must handle split sequences.
  //
  // Deliberately appended at the end of the struct: existing consumers that
  // aggregate-initialize the first ten members get nullptr here and keep the
  // legacy behaviour unchanged.
  void (*onStringChunk)(void* ctx, const char* data, size_t len);
  void (*onStringEnd)(void* ctx);
};

class StreamingJsonParser {
 public:
  static constexpr size_t TOKEN_BUF_SIZE = 512;
  static constexpr size_t MAX_NESTING = 32;

  explicit StreamingJsonParser(const JsonCallbacks& callbacks);

  void reset();
  void feed(const char* data, size_t len);

  bool hasError() const { return error; }

 private:
  enum class State : uint8_t {
    SCANNING,
    IN_STRING_KEY,
    IN_STRING_VALUE,
    IN_NUMBER,
    IN_LITERAL,
    SKIP_STRING,
  };

  enum class Container : uint8_t {
    NONE,
    OBJECT,
    ARRAY,
  };

  void handleScanning(char c);
  void handleStringChar(char c);
  void handleNumber(char c);
  void handleLiteral(char c);
  void handleSkipString(char c);

  void appendToken(char c);
  void emitToken();

  bool streamingValues() const { return cb.onStringChunk != nullptr && cb.onStringEnd != nullptr; }
  void flushValueChunk();

  void handleUnicodeHexDigit(char c);
  void emitCodepoint(uint32_t codepoint);
  void flushPendingSurrogate();

  bool inArray() const { return nestingDepth > 0 && nestingStack[nestingDepth - 1] == Container::ARRAY; }

  JsonCallbacks cb;
  char tokenBuf[TOKEN_BUF_SIZE];
  size_t tokenLen;
  State state;
  bool expectingValue;
  bool escaped;
  bool tokenOverflow;
  bool error;

  Container nestingStack[MAX_NESTING];
  uint8_t nestingDepth;

  char literalExpected[6];
  uint8_t literalLen;
  uint8_t literalPos;

  // \uXXXX escape decoding, incremental so a chunk boundary can land anywhere
  // inside the six escape characters. `unicodeDigits` > 0 while collecting hex;
  // `pendingSurrogate` holds a high surrogate awaiting its low half. An
  // unpaired surrogate decodes to U+FFFD rather than corrupting the stream.
  uint8_t unicodeDigits;
  uint32_t unicodeValue;
  uint32_t pendingSurrogate;
};
