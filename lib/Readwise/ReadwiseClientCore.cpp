#include "ReadwiseClientCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ReadwiseDocument.h"

namespace readwise {
namespace {

bool isUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

// Appends `value` percent-encoded. Returns false on overflow.
bool appendEncoded(char* out, size_t outCap, size_t& pos, const char* value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const char* p = value; *p != '\0'; ++p) {
    const char c = *p;
    if (isUnreserved(c)) {
      if (pos + 1 >= outCap) {
        return false;
      }
      out[pos++] = c;
    } else {
      if (pos + 3 >= outCap) {
        return false;
      }
      const uint8_t byte = static_cast<uint8_t>(c);
      out[pos++] = '%';
      out[pos++] = kHex[byte >> 4];
      out[pos++] = kHex[byte & 0x0F];
    }
  }
  return true;
}

bool appendLiteral(char* out, size_t outCap, size_t& pos, const char* text) {
  const size_t len = strlen(text);
  if (pos + len >= outCap) {
    return false;
  }
  memcpy(out + pos, text, len);
  pos += len;
  return true;
}

}  // namespace

bool buildListUrl(const ListQuery& query, bool withHtmlContent, char* out, size_t outCap) {
  size_t pos = 0;
  if (!appendLiteral(out, outCap, pos, API_BASE) || !appendLiteral(out, outCap, pos, "/list/?limit=")) {
    return false;
  }
  char limitBuf[8];
  snprintf(limitBuf, sizeof(limitBuf), "%u", static_cast<unsigned>(query.limit));
  if (!appendLiteral(out, outCap, pos, limitBuf)) {
    return false;
  }
  if (query.location != Location::Unknown) {
    if (!appendLiteral(out, outCap, pos, "&location=") ||
        !appendLiteral(out, outCap, pos, locationName(query.location))) {
      return false;
    }
  }
  if (query.updatedAfter != nullptr && query.updatedAfter[0] != '\0') {
    if (!appendLiteral(out, outCap, pos, "&updatedAfter=") || !appendEncoded(out, outCap, pos, query.updatedAfter)) {
      return false;
    }
  }
  if (query.pageCursor != nullptr && query.pageCursor[0] != '\0') {
    if (!appendLiteral(out, outCap, pos, "&pageCursor=") || !appendEncoded(out, outCap, pos, query.pageCursor)) {
      return false;
    }
  }
  if (withHtmlContent) {
    if (!appendLiteral(out, outCap, pos, "&withHtmlContent=true")) {
      return false;
    }
  }
  out[pos] = '\0';
  return true;
}

bool buildBodyUrl(const char* id, char* out, size_t outCap) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  size_t pos = 0;
  if (!appendLiteral(out, outCap, pos, API_BASE) || !appendLiteral(out, outCap, pos, "/list/?id=") ||
      !appendEncoded(out, outCap, pos, id) || !appendLiteral(out, outCap, pos, "&withHtmlContent=true")) {
    return false;
  }
  out[pos] = '\0';
  return true;
}

bool buildUpdateUrl(const char* id, char* out, size_t outCap) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  size_t pos = 0;
  if (!appendLiteral(out, outCap, pos, API_BASE) || !appendLiteral(out, outCap, pos, "/update/") ||
      !appendEncoded(out, outCap, pos, id) || !appendLiteral(out, outCap, pos, "/")) {
    return false;
  }
  out[pos] = '\0';
  return true;
}

bool buildUpdateBody(const PendingOp& op, char* out, size_t outCap) {
  if (op.op == OpType::SetSeen) {
    const int written = snprintf(out, outCap, "{\"seen\":%s}", op.payload != 0 ? "true" : "false");
    return written > 0 && static_cast<size_t>(written) < outCap;
  }
  if (op.op == OpType::SetLocation) {
    const Location location = static_cast<Location>(op.payload);
    // Only the writable set. Unknown must never reach the wire, and `feed` is
    // not a destination the device offers.
    if (location != Location::New && location != Location::Later && location != Location::Shortlist &&
        location != Location::Archive) {
      return false;
    }
    const int written = snprintf(out, outCap, "{\"location\":\"%s\"}", locationName(location));
    return written > 0 && static_cast<size_t>(written) < outCap;
  }
  return false;
}

ApiStatus statusFromHttp(int httpStatus) {
  if (httpStatus < 0) {
    return ApiStatus::NetworkError;
  }
  if (httpStatus == 200 || httpStatus == 201 || httpStatus == 204) {
    return ApiStatus::Ok;
  }
  if (httpStatus == 401 || httpStatus == 403) {
    return ApiStatus::AuthFailed;
  }
  if (httpStatus == 429) {
    return ApiStatus::RateLimited;
  }
  return ApiStatus::ServerError;
}

uint16_t parseRetryAfter(const char* headerValue) {
  if (headerValue == nullptr || headerValue[0] == '\0') {
    return 0;
  }
  char* end = nullptr;
  const long value = strtol(headerValue, &end, 10);
  // Trailing junk ("16junk") means the value cannot be trusted; 0 reads as
  // "no guidance" and the caller abandons rather than retrying blind.
  if (end == headerValue || (end != nullptr && *end != '\0')) {
    return 0;
  }
  if (value <= 0 || value > 0xFFFF) {
    return 0;
  }
  return static_cast<uint16_t>(value);
}

}  // namespace readwise
