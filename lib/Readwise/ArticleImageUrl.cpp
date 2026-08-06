#include "ArticleImageUrl.h"

#include <cstring>

namespace readwise {
namespace {

char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool startsWithIgnoreCase(const char* s, size_t len, const char* prefix) {
  const size_t prefixLen = strlen(prefix);
  if (len < prefixLen) {
    return false;
  }
  for (size_t i = 0; i < prefixLen; ++i) {
    if (toLowerAscii(s[i]) != prefix[i]) {
      return false;
    }
  }
  return true;
}

// Length of a "scheme:" prefix, or 0 if there is none. A colon only counts
// before the first '/', '?', or '#', so "a/b:c.jpg" is a relative path rather
// than a scheme.
size_t schemeLength(const char* s, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = s[i];
    if (c == ':') {
      return i > 0 ? i + 1 : 0;
    }
    if (c == '/' || c == '?' || c == '#') {
      return 0;
    }
  }
  return 0;
}

// Everything before the path: "https://host:port".
size_t originLength(const char* url, size_t len) {
  const size_t scheme = schemeLength(url, len);
  if (scheme == 0 || scheme + 2 > len || url[scheme] != '/' || url[scheme + 1] != '/') {
    return 0;
  }
  for (size_t i = scheme + 2; i < len; ++i) {
    if (url[i] == '/' || url[i] == '?' || url[i] == '#') {
      return i;
    }
  }
  return len;
}

// Everything up to and including the last '/' of the path, with any query or
// fragment discarded first.
size_t directoryLength(const char* url, size_t len) {
  const size_t origin = originLength(url, len);
  if (origin == 0) {
    return 0;
  }
  size_t end = len;
  for (size_t i = origin; i < len; ++i) {
    if (url[i] == '?' || url[i] == '#') {
      end = i;
      break;
    }
  }
  for (size_t i = end; i > origin; --i) {
    if (url[i - 1] == '/') {
      return i;
    }
  }
  return origin;
}

struct Builder {
  char* out;
  size_t cap;
  size_t len = 0;
  bool overflow = false;

  void append(const char* data, size_t n) {
    if (overflow || len + n + 1 > cap) {
      overflow = true;
      return;
    }
    memcpy(out + len, data, n);
    len += n;
  }
  void append(const char* s) { append(s, strlen(s)); }
};

// Collapses "." and ".." path segments. Applied after joining so that a
// relative src like "../img/a.jpg" resolves the way a browser would.
size_t normalizePath(char* url, size_t len) {
  const size_t origin = originLength(url, len);
  if (origin == 0 || origin >= len) {
    return len;
  }
  size_t write = origin;
  size_t read = origin;
  while (read < len) {
    // Copy the leading '/' of this segment, then inspect the segment itself.
    if (url[read] == '/') {
      // "/./" collapses away entirely.
      if (read + 1 < len && url[read + 1] == '.' &&
          (read + 2 == len || url[read + 2] == '/' || url[read + 2] == '?' || url[read + 2] == '#')) {
        read += 2;
        if (read == len) {
          url[write++] = '/';
        }
        continue;
      }
      // "/../" pops the previous segment, never past the origin.
      if (read + 2 < len && url[read + 1] == '.' && url[read + 2] == '.' &&
          (read + 3 == len || url[read + 3] == '/' || url[read + 3] == '?' || url[read + 3] == '#')) {
        while (write > origin && url[write - 1] != '/') {
          --write;
        }
        if (write > origin) {
          --write;
        }
        read += 3;
        if (read == len) {
          url[write++] = '/';
        }
        continue;
      }
    }
    // Query and fragment are opaque; stop normalizing at their start.
    if (url[read] == '?' || url[read] == '#') {
      break;
    }
    url[write++] = url[read++];
  }
  while (read < len) {
    url[write++] = url[read++];
  }
  return write;
}

// Extensions we know we cannot decode. Everything else is accepted and settled
// by magic-byte sniffing after download, since a great many image URLs carry
// no extension at all.
bool hasUndecodableExtension(const char* url, size_t len) {
  static const char* const kRejected[] = {".svg", ".gif", ".webp", ".avif", ".bmp", ".ico", ".tif", ".tiff"};

  size_t end = len;
  for (size_t i = 0; i < len; ++i) {
    if (url[i] == '?' || url[i] == '#') {
      end = i;
      break;
    }
  }
  size_t dot = end;
  while (dot > 0 && url[dot - 1] != '.' && url[dot - 1] != '/') {
    --dot;
  }
  if (dot == 0 || url[dot - 1] != '.') {
    return false;
  }
  const char* ext = url + dot - 1;
  const size_t extLen = end - dot + 1;
  for (const char* rejected : kRejected) {
    if (extLen == strlen(rejected) && startsWithIgnoreCase(ext, extLen, rejected)) {
      return true;
    }
  }
  return false;
}

// A declared dimension of 1 or 0 marks a tracking beacon, not content.
bool isTinyDimension(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  int number = 0;
  for (const char* p = value; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      // A unit suffix ("2px") still counts; anything else ("100%") does not.
      return (*p == 'p' || *p == 'P') && number <= 2 && number > 0;
    }
    number = number * 10 + (*p - '0');
    if (number > 2) {
      return false;
    }
  }
  return number <= 2;
}

}  // namespace

ImageUrlResult resolveArticleImageUrl(const char* baseUrl, const char* src, const char* width, const char* height,
                                      char* out, size_t outCap) {
  if (outCap == 0) {
    return ImageUrlResult::TooLong;
  }
  out[0] = '\0';

  if (src == nullptr) {
    return ImageUrlResult::NoSource;
  }
  // Attribute values routinely carry surrounding whitespace and newlines.
  while (*src != '\0' && isSpace(*src)) {
    ++src;
  }
  size_t srcLen = strlen(src);
  while (srcLen > 0 && isSpace(src[srcLen - 1])) {
    --srcLen;
  }
  if (srcLen == 0) {
    return ImageUrlResult::NoSource;
  }

  if (isTinyDimension(width) || isTinyDimension(height)) {
    return ImageUrlResult::TrackingPixel;
  }

  const size_t baseLen = baseUrl != nullptr ? strlen(baseUrl) : 0;
  Builder builder{out, outCap};

  if (srcLen >= 2 && src[0] == '/' && src[1] == '/') {
    // Protocol-relative: inherit the base scheme, defaulting to https.
    const size_t baseScheme = schemeLength(baseUrl, baseLen);
    if (baseScheme > 0) {
      builder.append(baseUrl, baseScheme);
    } else {
      builder.append("https:");
    }
    builder.append(src, srcLen);
  } else if (const size_t scheme = schemeLength(src, srcLen); scheme > 0) {
    if (!startsWithIgnoreCase(src, srcLen, "http://") && !startsWithIgnoreCase(src, srcLen, "https://")) {
      // data:, javascript:, blob:, mailto: -- nothing fetchable.
      return ImageUrlResult::NotHttp;
    }
    builder.append(src, srcLen);
  } else if (src[0] == '/') {
    const size_t origin = originLength(baseUrl, baseLen);
    if (origin == 0) {
      return ImageUrlResult::Unresolvable;
    }
    builder.append(baseUrl, origin);
    builder.append(src, srcLen);
  } else {
    const size_t directory = directoryLength(baseUrl, baseLen);
    if (directory == 0) {
      return ImageUrlResult::Unresolvable;
    }
    builder.append(baseUrl, directory);
    if (baseUrl[directory - 1] != '/') {
      builder.append("/", 1);
    }
    builder.append(src, srcLen);
  }

  if (builder.overflow) {
    // The builder stopped appending but left partial bytes behind; a truncated
    // URL would fetch the wrong thing, so leave nothing at all.
    out[0] = '\0';
    return ImageUrlResult::TooLong;
  }

  // A fragment is meaningless to an image fetch and would corrupt the request.
  for (size_t i = 0; i < builder.len; ++i) {
    if (out[i] == '#') {
      builder.len = i;
      break;
    }
  }

  builder.len = normalizePath(out, builder.len);
  out[builder.len] = '\0';

  if (hasUndecodableExtension(out, builder.len)) {
    out[0] = '\0';
    return ImageUrlResult::UnsupportedType;
  }
  return ImageUrlResult::Accepted;
}

}  // namespace readwise
