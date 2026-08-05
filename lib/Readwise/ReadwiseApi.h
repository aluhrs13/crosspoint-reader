#pragma once

#include <cstdint>

#include "ReadwiseDocument.h"

// The seam between the sync engine and the network.
//
// Phase 2 implements the whole sync pipeline against this interface and ships
// only an in-memory fake; the real client lands in #4, because today's
// HttpDownloader cannot send a bearer token, surface a status code, or read the
// retry-after header, and StreamingJsonParser silently drops strings over 512
// bytes.
//
// Keeping the seam here is also what makes issue #3's acceptance criteria
// testable: interrupted sync, checkpoint recovery, retries, and conflict
// resolution are all exercised on the host.

namespace readwise {

// Modelled on KOReaderSyncClient::Error, which is the established shape for a
// sync client result in this codebase.
enum class ApiStatus : uint8_t {
  Ok = 0,
  NoCredentials,
  AuthFailed,
  NetworkError,
  RateLimited,
  LowMemory,
  ParseError,
  ServerError,
};

const char* apiStatusName(ApiStatus status);

// Cursors are opaque ULIDs on the wire; sized to match the timestamp fields.
inline constexpr int CURSOR_CAP = 33;

struct ListQuery {
  // Verbatim ISO 8601 from the last committed checkpoint. Empty means "no
  // cursor yet", i.e. a full pull of the requested location.
  const char* updatedAfter = "";
  Location location = Location::Unknown;
  // Empty on the first page; thereafter the previous response's nextPageCursor.
  const char* pageCursor = "";
  uint16_t limit = 100;
};

struct ListResponse {
  ApiStatus status = ApiStatus::Ok;
  // Empty when this was the final page. Only a null cursor means complete --
  // `count` saturates at 10,000 and can never signal completion.
  char nextPageCursor[CURSOR_CAP] = {};
  // Populated only for RateLimited. Read from the `retry-after` header, whose
  // name is lowercase on the wire, so the client's lookup must be
  // case-insensitive.
  uint16_t retryAfterSeconds = 0;
};

// Receives documents one at a time so no implementation may buffer a page: a
// 100-document page is ~135 KB against a ~380 KB ceiling.
class DocumentSink {
 public:
  virtual ~DocumentSink() = default;
  // Return false to abort the fetch mid-page.
  virtual bool onDocument(const Document& doc) = 0;
};

// Receives an article's html_content in chunks as it decodes from the JSON
// stream. A body is ~88 KB observed, so it can never be delivered whole.
// Chunks split at arbitrary byte positions, including inside UTF-8 sequences.
class BodySink {
 public:
  virtual ~BodySink() = default;
  virtual bool onBodyChunk(const char* data, size_t len) = 0;
  // `complete` is false when the transfer ended before the string closed; the
  // sink must then discard rather than commit partial text.
  virtual bool onBodyEnd(bool complete) = 0;
};

class ReadwiseApi {
 public:
  virtual ~ReadwiseApi() = default;

  // Fetches one page and streams its documents to `sink`.
  virtual ListResponse fetchPage(const ListQuery& query, DocumentSink& sink) = 0;

  // Applies one pending operation. Only SetLocation and SetSeen are ever
  // queued: PATCH /update/ answers 200 for reading_progress and silently
  // discards it, so progress is never pushed.
  //
  // Note that a 200 is not evidence a field was written -- the endpoint returns
  // 200 for fields it ignores -- so an implementation that needs certainty must
  // read the document back.
  virtual ApiStatus pushOp(const struct PendingOp& op) = 0;

  // Fetches one document with withHtmlContent=true and streams the body to
  // `sink`. Used by the sync pass to prefetch every missing body, and
  // on-demand as the retry path when a document is opened. `retryAfterSeconds`
  // is set on RateLimited.
  virtual ApiStatus fetchBody(const char* id, BodySink& sink, uint16_t* retryAfterSeconds) = 0;
};

}  // namespace readwise
