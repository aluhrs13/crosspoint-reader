#include "HttpReadwiseApi.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>

#include <cstring>
#include <utility>

#include "ReadwiseClientCore.h"
#include "ReadwiseListParser.h"

namespace readwise {
namespace {

// Same thresholds and rationale as KOReaderSyncClient.cpp:46 -- the wolfSSL
// handshake needs ~30-40 KB transient, MEMORY_E fails soft, and the gate exists
// to avoid wasting a 15-60 s timeout on a doomed attempt, not as a guarantee.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

// The list endpoint answers with Connection: close and no Content-Length
// (chunked), so responses can take a while on a slow AP; match HttpDownloader's
// 60 s rather than SecureHttpClient's 15 s default.
constexpr uint32_t HTTP_TIMEOUT_MS = 60000;

// Longest URL: /list/ with an encoded updatedAfter (~96 encoded bytes) plus a
// cursor. 256 leaves ample slack.
constexpr size_t URL_CAP = 256;

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_FOR_TLS || maxAllocHeap < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("RWAPI", "Insufficient heap for TLS: %u free (need %u), %u max alloc (need %u)", freeHeap, MIN_FREE_FOR_TLS,
            maxAllocHeap, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

void configureClient(freeink::SecureHttpClient& http, const std::string& token) {
  // TLS posture: unverified, matching every existing call site. Recorded in
  // docs/readwise-api-contract.md; setCACert() is the follow-up path.
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent("CrossPoint-ESP32-Readwise");
  http.addHeader("Authorization", std::string("Token ") + token);
}

uint16_t retryAfterFrom(const freeink::SecureHttpClient& http) {
  // SecureHttpClient lowercases header names on receipt and getHeader()
  // lowercases the lookup, so the server's lowercase "retry-after" is found
  // regardless of spelling.
  return parseRetryAfter(http.getHeader("retry-after").c_str());
}

}  // namespace

HttpReadwiseApi::HttpReadwiseApi(std::string token) : token(std::move(token)) {}

ApiStatus HttpReadwiseApi::checkAuth() {
  if (token.empty()) {
    return ApiStatus::NoCredentials;
  }
  if (insufficientHeap()) {
    return ApiStatus::LowMemory;
  }
  freeink::SecureHttpClient http;
  if (!http.begin(AUTH_CHECK_URL)) {
    return ApiStatus::NetworkError;
  }
  configureClient(http, token);
  const int status = http.GET();
  http.end();
  LOG_DBG("RWAPI", "Auth check: %d", status);
  return statusFromHttp(status);
}

ApiStatus HttpReadwiseApi::runListRequest(const char* url, DocumentSink& docSink, BodySink* bodySink, char* cursorOut,
                                          uint16_t* retryAfterSeconds) {
  if (token.empty()) {
    return ApiStatus::NoCredentials;
  }
  if (insufficientHeap()) {
    return ApiStatus::LowMemory;
  }

  // ~1.5 KB (Document + token buffer + field buffer): far over the 256-byte
  // stack guidance, and stacking it on top of the live TLS session's frames
  // would be exactly the overflow the guidance exists to prevent.
  auto parser = makeUniqueNoThrow<ReadwiseListParser>(docSink, bodySink);
  if (!parser) {
    LOG_ERR("RWAPI", "OOM: list parser");
    return ApiStatus::LowMemory;
  }

  freeink::SecureHttpClient http;
  if (!http.begin(url)) {
    LOG_ERR("RWAPI", "Bad URL: %s", url);
    return ApiStatus::NetworkError;
  }
  configureClient(http, token);

  ReadwiseListParser* parserPtr = parser.get();
  const int status = http.GET([parserPtr](const uint8_t* data, size_t len) {
    // Returning false aborts the transfer as soon as parsing fails or a sink
    // says stop; no point paying for bytes nothing will consume.
    return parserPtr->feed(reinterpret_cast<const char*>(data), len);
  });

  LOG_DBG("RWAPI", "GET %s -> %d (heap %u)", url, status, (unsigned)ESP.getFreeHeap());

  if (status == 429 && retryAfterSeconds != nullptr) {
    *retryAfterSeconds = retryAfterFrom(http);
  }
  http.end();

  const ApiStatus mapped = statusFromHttp(status);
  if (mapped != ApiStatus::Ok) {
    return mapped;
  }
  // A 200 whose body did not parse, arrived truncated at the HTTP layer, or
  // never closed its JSON envelope (fully framed but cut-short content) is a
  // parse failure, not a success with fewer documents -- treating it as final
  // could commit an incomplete page as a completed sweep.
  if (parser->hasError() || parser->wasAborted() || !http.responseComplete() || !parser->complete()) {
    LOG_ERR("RWAPI", "List response invalid: parseError=%d aborted=%d httpComplete=%d jsonComplete=%d",
            parser->hasError(), parser->wasAborted(), http.responseComplete(), parser->complete());
    return ApiStatus::ParseError;
  }
  if (cursorOut != nullptr) {
    // Empty when the response said null -- the only completion signal.
    strncpy(cursorOut, parser->nextPageCursor(), CURSOR_CAP - 1);
    cursorOut[CURSOR_CAP - 1] = '\0';
  }
  return ApiStatus::Ok;
}

ListResponse HttpReadwiseApi::fetchPage(const ListQuery& query, DocumentSink& sink) {
  ListResponse response;
  char url[URL_CAP];
  if (!buildListUrl(query, /*withHtmlContent=*/false, url, sizeof(url))) {
    response.status = ApiStatus::NetworkError;
    return response;
  }
  response.status = runListRequest(url, sink, nullptr, response.nextPageCursor, &response.retryAfterSeconds);
  return response;
}

ApiStatus HttpReadwiseApi::fetchBody(const char* id, BodySink& sink, uint16_t* retryAfterSeconds) {
  char url[URL_CAP];
  if (!buildBodyUrl(id, url, sizeof(url))) {
    return ApiStatus::NetworkError;
  }
  // The single-document response still arrives through the list envelope, so
  // the same parser handles it; the documents themselves go to a discarding
  // sink because only the body matters here.
  struct DiscardDocs : DocumentSink {
    bool onDocument(const Document&) override { return true; }
  } discard;
  return runListRequest(url, discard, &sink, nullptr, retryAfterSeconds);
}

ApiStatus HttpReadwiseApi::createHighlights(const HighlightPayload* items, size_t count, uint16_t* retryAfterSeconds) {
  if (retryAfterSeconds != nullptr) {
    *retryAfterSeconds = 0;
  }
  if (token.empty()) {
    return ApiStatus::NoCredentials;
  }
  if (insufficientHeap()) {
    return ApiStatus::LowMemory;
  }

  // A 16-item batch of 1 KB highlights plus escaping overhead fits well inside
  // 20 KB. Heap, transient for this call only; the caller already caps batch
  // text at 8 KB.
  constexpr size_t BODY_CAP = 20 * 1024;
  auto body = makeUniqueNoThrow<char[]>(BODY_CAP);
  if (!body) {
    LOG_ERR("RWAPI", "OOM: highlights body");
    return ApiStatus::LowMemory;
  }
  if (!buildHighlightsCreateBody(items, count, body.get(), BODY_CAP)) {
    LOG_ERR("RWAPI", "Highlights body build failed (%u items)", (unsigned)count);
    return ApiStatus::ServerError;
  }

  freeink::SecureHttpClient http;
  if (!http.begin(HIGHLIGHTS_CREATE_URL)) {
    return ApiStatus::NetworkError;
  }
  configureClient(http, token);
  http.addHeader("Content-Type", "application/json");
  const int status = http.sendRequest("POST", body.get());
  if (status == 429 && retryAfterSeconds != nullptr) {
    *retryAfterSeconds = retryAfterFrom(http);
  }
  http.end();

  LOG_DBG("RWAPI", "POST highlights (%u items) -> %d", (unsigned)count, status);
  return statusFromHttp(status);
}

ApiStatus HttpReadwiseApi::pushOp(const PendingOp& op) {
  if (token.empty()) {
    return ApiStatus::NoCredentials;
  }
  char url[URL_CAP];
  char body[64];
  if (!buildUpdateUrl(op.id, url, sizeof(url)) || !buildUpdateBody(op, body, sizeof(body))) {
    LOG_ERR("RWAPI", "Unpushable op %u for %s", static_cast<unsigned>(op.op), op.id);
    return ApiStatus::ServerError;
  }
  if (insufficientHeap()) {
    return ApiStatus::LowMemory;
  }

  freeink::SecureHttpClient http;
  if (!http.begin(url)) {
    return ApiStatus::NetworkError;
  }
  configureClient(http, token);
  http.addHeader("Content-Type", "application/json");
  const int status = http.sendRequest("PATCH", body);
  http.end();

  LOG_DBG("RWAPI", "PATCH %s -> %d", url, status);
  return statusFromHttp(status);
}

}  // namespace readwise
