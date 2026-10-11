#pragma once

#include <string>

#include "ReadwiseApi.h"

// The on-device ReadwiseApi, speaking to readwise.io over freeink's
// SecureHttpClient. Everything protocol-shaped (URLs, PATCH bodies, status
// mapping) lives in ReadwiseClientCore so it is host-tested; this class owns
// only the transport wiring: the token header, heap gating before each TLS
// handshake, streaming the response into ReadwiseListParser, and reading the
// lowercase retry-after header.
//
// TLS: setInsecure(), matching every existing SecureHttpClient call site. The
// decision and its risk record live in docs/readwise-api-contract.md
// ("Transport"); SecureHttpClient::setCACert() exists as a follow-up path.

namespace readwise {

class HttpReadwiseApi : public ReadwiseApi {
 public:
  explicit HttpReadwiseApi(std::string token);

  // GET /api/v2/auth/ -- 204 means the token is valid. Cheap connectivity and
  // credential check for the settings UI.
  ApiStatus checkAuth();

  ListResponse fetchPage(const ListQuery& query, DocumentSink& sink) override;
  ApiStatus pushHighlight(const char* text, const char* title, const char* author, const char* sourceUrl) override;
  ApiStatus pushOp(const PendingOp& op) override;
  ApiStatus fetchBody(const char* id, BodySink& sink, uint16_t* retryAfterSeconds) override;

  const char* lastDetail() const override { return detail_; }

 private:
  ApiStatus runListRequest(const char* url, DocumentSink& docSink, BodySink* bodySink, char* cursorOut,
                           uint16_t* retryAfterSeconds);
  // Records a failed response. A 2xx clears the previous trace.
  void noteHttpResult(int status, const char* body, size_t bodyLen);
  void noteLocal(const char* text);

  std::string token;
  // Status plus a short body snippet. Lives on the client, not the stack of
  // the request, and is capped so an HTML error page cannot be retained.
  static constexpr size_t DETAIL_CAP = 96;
  char detail_[DETAIL_CAP] = {};
};

}  // namespace readwise
