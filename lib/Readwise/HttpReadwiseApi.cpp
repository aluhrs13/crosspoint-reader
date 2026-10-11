#include "HttpReadwiseApi.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "ReadwiseClientCore.h"
#include "ReadwiseHighlight.h"
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

// First bytes of an error body. Enough for Readwise's `{"detail":...}`.
constexpr size_t ERROR_SNIP_CAP = 80;

uint16_t retryAfterFrom(const freeink::SecureHttpClient& http) {
  // SecureHttpClient lowercases header names on receipt and getHeader()
  // lowercases the lookup, so the server's lowercase "retry-after" is found
  // regardless of spelling.
  return parseRetryAfter(http.getHeader("retry-after").c_str());
}

}  // namespace

HttpReadwiseApi::HttpReadwiseApi(std::string token) : token(std::move(token)) {}

void HttpReadwiseApi::noteLocal(const char* text) {
  snprintf(detail_, sizeof(detail_), "%s", text != nullptr ? text : "");
  LOG_ERR("RWAPI", "%s", detail_);
}

void HttpReadwiseApi::noteHttpResult(int status, const char* body, size_t bodyLen) {
  detail_[0] = '\0';
  const ApiStatus mapped = statusFromHttp(status);
  if (mapped == ApiStatus::Ok) {
    return;
  }
  formatHttpDetail(status, body, bodyLen, detail_, sizeof(detail_));
  if (mapped == ApiStatus::ServerError || mapped == ApiStatus::NotFound || mapped == ApiStatus::Rejected) {
    LOG_ERR("RWAPI", "%s", detail_);
  }
}

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
  // Error bodies are small JSON. Keep a prefix so a 4xx/5xx can be shown;
  // the success path streams into the parser and does not retain this.
  char snip[ERROR_SNIP_CAP];
  size_t snipLen = 0;
  char* snipPtr = snip;
  const int status = http.GET([parserPtr, snipPtr, &snipLen](const uint8_t* data, size_t len) {
    if (snipLen + 1 < ERROR_SNIP_CAP) {
      const size_t room = ERROR_SNIP_CAP - 1 - snipLen;
      const size_t n = len < room ? len : room;
      memcpy(snipPtr + snipLen, data, n);
      snipLen += n;
      snipPtr[snipLen] = '\0';
    }
    // Returning false aborts the transfer as soon as parsing fails or a sink
    // says stop; no point paying for bytes nothing will consume.
    return parserPtr->feed(reinterpret_cast<const char*>(data), len);
  });

  LOG_DBG("RWAPI", "GET %s -> %d (heap %u)", url, status, (unsigned)ESP.getFreeHeap());
  noteHttpResult(status, snip, snipLen);

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

ApiStatus HttpReadwiseApi::pushHighlight(const char* text, const char* title, const char* author,
                                         const char* sourceUrl) {
  detail_[0] = '\0';
  if (token.empty()) {
    return ApiStatus::NoCredentials;
  }
  if (text == nullptr || text[0] == '\0') {
    noteLocal("empty highlight");
    return ApiStatus::ServerError;
  }
  // Escaping can double the quote, title, author, and URL. 2 KB covers that
  // plus the JSON keys; the quote itself is capped at 280 bytes.
  constexpr size_t kBodyCap = 2048;
  auto body = makeUniqueNoThrow<char[]>(kBodyCap);
  if (!body) {
    LOG_ERR("RWAPI", "OOM: highlight body");
    return ApiStatus::LowMemory;
  }
  if (!buildHighlightBody(text, title, author, sourceUrl, body.get(), kBodyCap)) {
    noteLocal("highlight body too large");
    return ApiStatus::ServerError;
  }
  if (insufficientHeap()) {
    return ApiStatus::LowMemory;
  }
  freeink::SecureHttpClient http;
  if (!http.begin(HIGHLIGHTS_URL)) {
    return ApiStatus::NetworkError;
  }
  configureClient(http, token);
  http.addHeader("Content-Type", "application/json");
  const int status = http.sendRequest("POST", body.get());
  // getString() is std::string on device and Arduino String in the simulator.
  // Both expose c_str() and length(). The temporary lives for this call.
  noteHttpResult(status, http.getString().c_str(), http.getString().length());
  http.end();
  LOG_DBG("RWAPI", "POST highlight -> %d", status);
  return statusFromHttp(status);
}

ApiStatus HttpReadwiseApi::pushOp(const PendingOp& op) {
  detail_[0] = '\0';
  if (token.empty()) {
    return ApiStatus::NoCredentials;
  }
  char url[URL_CAP];
  if (op.op == OpType::Delete) {
    if (!buildDeleteUrl(op.id, url, sizeof(url))) {
      noteLocal("unpushable delete");
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
    const int status = http.sendRequest("DELETE", "");
    if (status != 404) {
      noteHttpResult(status, http.getString().c_str(), http.getString().length());
    }
    http.end();
    LOG_DBG("RWAPI", "DELETE %s -> %d", url, status);
    // A repeated delete of a document the server already removed. Treating it
    // as success lets the journal entry drain instead of stalling the queue.
    if (status == 404) {
      return ApiStatus::Ok;
    }
    return statusFromHttp(status);
  }

  char body[64];
  if (!buildUpdateUrl(op.id, url, sizeof(url)) || !buildUpdateBody(op, body, sizeof(body))) {
    noteLocal("unpushable update");
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
  noteHttpResult(status, http.getString().c_str(), http.getString().length());
  http.end();

  LOG_DBG("RWAPI", "PATCH %s -> %d", url, status);
  return statusFromHttp(status);
}

}  // namespace readwise
