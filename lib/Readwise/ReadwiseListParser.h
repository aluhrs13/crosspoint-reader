#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

#include "ReadwiseApi.h"
#include "ReadwiseDocument.h"

// Streaming parser for GET /api/v3/list/ responses.
//
// Follows the ReleaseJsonParser pattern -- a small state machine over
// StreamingJsonParser with fixed buffers -- but uses the parser's chunked
// string sink, because `html_content` (~88 KB observed) and `summary` (2,093
// bytes observed) both exceed the 512-byte token buffer that the legacy
// callbacks silently drop at.
//
// Documents are delivered one at a time through a DocumentSink as their closing
// brace arrives; `html_content` bytes are routed straight to an optional
// BodySink without ever being buffered whole. Total memory is one Document plus
// one summary-sized field buffer, regardless of response size.
//
// This object is ~1.5 KB (Document + parser token buffer + field buffer), far
// over the 256-byte stack guidance -- callers heap-allocate it once per
// operation with makeUniqueNoThrow and reuse it across pages.

namespace readwise {

class ReadwiseListParser {
 public:
  // `documents` receives each parsed document; `body` may be null when
  // withHtmlContent was not requested.
  ReadwiseListParser(DocumentSink& documents, BodySink* body);

  void reset();
  // Returns false once parsing has failed or a sink aborted; the HTTP data
  // callback should then abort the transfer.
  bool feed(const char* data, size_t len);

  bool hasError() const { return parser.hasError() || malformed; }
  bool wasAborted() const { return aborted; }
  // True once the response envelope's closing brace has been seen. A transfer
  // whose HTTP framing completed but whose JSON was cut short parses without
  // error yet never closes the envelope -- the client must treat that as a
  // failed page, or a truncated response would commit as a complete sweep.
  bool complete() const { return envelopeClosed && !hasError(); }

  // Response envelope. The cursor is empty on the final page; `count` saturates
  // at 10,000 server-side and must never signal completion.
  const char* nextPageCursor() const { return cursor; }
  bool sawNextPageCursorKey() const { return sawCursorKey; }
  uint32_t count() const { return totalCount; }
  uint16_t documentsDelivered() const { return delivered; }

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_RESULTS,
    IN_DOCUMENT,
  };

  enum class LastKey : uint8_t {
    NONE,
    ID,
    TITLE,
    AUTHOR,
    SITE_NAME,
    SUMMARY,
    SOURCE_URL,
    UPDATED_AT,
    LAST_MOVED_AT,
    WORD_COUNT,
    LOCATION,
    CATEGORY,
    READING_PROGRESS,
    FIRST_OPENED_AT,
    HTML_CONTENT,
    COUNT,
    NEXT_PAGE_CURSOR,
    RESULTS,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnStringChunk(void* ctx, const char* data, size_t len);
  static void sOnStringEnd(void* ctx);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void onKey(const char* key, size_t len);
  void onStringChunk(const char* data, size_t len);
  void onStringEnd();
  void onNumber(const char* value, size_t len);
  void onNull();
  void onObjectStart();
  void onObjectEnd();
  void onArrayStart();
  void onArrayEnd();

  void dispatchField();

  // Sized for the largest bounded field (summary).
  static constexpr size_t FIELD_CAP = SUMMARY_CAP;

  DocumentSink& documentSink;
  BodySink* bodySink;
  StreamingJsonParser parser;

  Position position = Position::TOP_LEVEL;
  LastKey lastKey = LastKey::NONE;
  // Depth of containers to skip inside the current scope (tags object, unknown
  // nested arrays). While positive, keys and values are structurally ignored.
  uint8_t skipDepth = 0;
  bool inBody = false;
  bool bodyOpen = false;
  bool aborted = false;
  bool malformed = false;
  bool sawCursorKey = false;
  bool envelopeOpen = false;
  bool envelopeClosed = false;

  Document doc;
  char fieldBuf[FIELD_CAP];
  size_t fieldLen = 0;
  bool fieldTruncated = false;

  char cursor[CURSOR_CAP] = {};
  uint32_t totalCount = 0;
  uint16_t delivered = 0;
};

}  // namespace readwise
