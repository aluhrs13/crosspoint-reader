#pragma once

#include <cstddef>
#include <cstdint>

#include "ReadwiseApi.h"
#include "ReadwiseCodec.h"

// The pure, host-testable half of the HTTP client: URL construction, request
// body construction, and status-code mapping. HttpReadwiseApi (device-only)
// composes these with SecureHttpClient; the native tests exercise them
// directly, which is how issue #4's HTTP-error criteria are covered without a
// network.

namespace readwise {

inline constexpr const char* API_BASE = "https://readwise.io/api/v3";
inline constexpr const char* AUTH_CHECK_URL = "https://readwise.io/api/v2/auth/";
// Highlight creation lives on the older Highlights API, not Reader v3.
inline constexpr const char* HIGHLIGHTS_CREATE_URL = "https://readwise.io/api/v2/highlights/";

// Builds the /list/ URL for a page query. Percent-encodes parameter values --
// updatedAfter carries '+' and ':' from ISO 8601, and an unencoded '+' decodes
// server-side as a space, silently shifting the sync window. Returns false if
// the buffer is too small.
bool buildListUrl(const ListQuery& query, bool withHtmlContent, char* out, size_t outCap);

// /list/?id=<id>&withHtmlContent=true for the on-demand body fetch.
bool buildBodyUrl(const char* id, char* out, size_t outCap);

// /update/<id>/ for a queued operation.
bool buildUpdateUrl(const char* id, char* out, size_t outCap);

// The PATCH payload for a pending op: {"location":"later"} or {"seen":true}.
// Returns false for an op that must never be pushed (unknown type, or a
// location value outside the writable set).
bool buildUpdateBody(const PendingOp& op, char* out, size_t outCap);

// The POST body for /api/v2/highlights/: {"highlights":[{...},...]}. Text and
// title are JSON-escaped; empty title/source_url are omitted. Returns false on
// overflow, a null/empty text, or count == 0 -- never a truncated body.
// Field set pinned by probe 13b: text, title, source_url, source_type,
// category; location and highlighted_at are deliberately omitted (no reliable
// RTC, no meaningful location for extracted plain text).
bool buildHighlightsCreateBody(const HighlightPayload* items, size_t count, char* out, size_t outCap);

// Maps an HTTP status (or a negative transport failure) to ApiStatus.
ApiStatus statusFromHttp(int httpStatus);

// Parses a retry-after header value. Returns 0 when absent or malformed; the
// caller treats 0 as "no guidance" and abandons rather than retrying blind.
uint16_t parseRetryAfter(const char* headerValue);

}  // namespace readwise
