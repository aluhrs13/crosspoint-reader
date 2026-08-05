#include "ReadwiseDocument.h"

#include <cstring>

namespace readwise {
namespace {

bool matches(const char* value, size_t len, const char* literal) {
  const size_t literalLen = strlen(literal);
  return len == literalLen && memcmp(value, literal, literalLen) == 0;
}

}  // namespace

Location parseLocation(const char* value, size_t len) {
  if (value == nullptr) {
    return Location::Unknown;
  }
  if (matches(value, len, "new")) return Location::New;
  if (matches(value, len, "later")) return Location::Later;
  if (matches(value, len, "shortlist")) return Location::Shortlist;
  if (matches(value, len, "archive")) return Location::Archive;
  if (matches(value, len, "feed")) return Location::Feed;
  return Location::Unknown;
}

Category parseCategory(const char* value, size_t len) {
  if (value == nullptr) {
    return Category::Unknown;
  }
  if (matches(value, len, "article")) return Category::Article;
  if (matches(value, len, "rss")) return Category::Rss;
  if (matches(value, len, "email")) return Category::Email;
  if (matches(value, len, "tweet")) return Category::Tweet;
  if (matches(value, len, "pdf")) return Category::Pdf;
  if (matches(value, len, "video")) return Category::Video;
  if (matches(value, len, "highlight")) return Category::Highlight;
  if (matches(value, len, "note")) return Category::Note;
  if (matches(value, len, "epub")) return Category::Epub;
  return Category::Unknown;
}

const char* locationName(Location location) {
  switch (location) {
    case Location::New:
      return "new";
    case Location::Later:
      return "later";
    case Location::Shortlist:
      return "shortlist";
    case Location::Archive:
      return "archive";
    case Location::Feed:
      return "feed";
    case Location::Unknown:
      break;
  }
  return "unknown";
}

void copyBounded(char* dst, size_t dstCap, const char* src, size_t srcLen) {
  if (dst == nullptr || dstCap == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  const size_t copyLen = srcLen < dstCap - 1 ? srcLen : dstCap - 1;
  memcpy(dst, src, copyLen);
  dst[copyLen] = '\0';
}

}  // namespace readwise
