#pragma once

#include <cstddef>
#include <cstdint>

// Which <img> tags in an article body are worth downloading, and what absolute
// URL to fetch them from.
//
// Pure string work, deliberately separated from the XHTML writer so it can be
// tested against the URL shapes real publishers emit without standing up a
// tokenizer.
//
// The accept rule is "anything that might be a JPEG or PNG", not "anything
// whose URL ends in .jpg". Extensionless CDN URLs (cdn.site/img/abc?w=800) are
// common and would otherwise silently lose their images. Since the local
// filename is written into the XHTML before the bytes exist, the real format
// is settled after download by sniffing magic bytes and patching the
// extension -- see ArticleXhtmlWriter's extension-offset reporting.

namespace readwise {

// Bounded by HtmlAttrCapture::SRC_CAP (512) plus room for a base origin.
inline constexpr size_t IMAGE_URL_CAP = 640;

// The archive holds images at OEBPS/images/<n>.<ext>, so this caps both the
// SD footprint and the ZipWriter entry count.
inline constexpr size_t MAX_ARTICLE_IMAGES = 24;

enum class ImageUrlResult : uint8_t {
  Accepted,
  NoSource,         // absent, empty, or dropped for being over-long
  Unresolvable,     // relative src with no usable base URL
  NotHttp,          // data:, javascript:, blob:, mailto: ...
  UnsupportedType,  // .svg/.gif/.webp/.avif -- no decoder on device
  TrackingPixel,    // declared 1x1 (or smaller); never content
  TooLong,          // resolved URL would not fit IMAGE_URL_CAP
};

// Resolves `src` against `baseUrl` (the document's source_url) and applies the
// accept policy. `out` receives a null-terminated absolute URL on Accepted and
// is left empty otherwise. `width`/`height` are the declared attribute values,
// or nullptr/empty when absent.
ImageUrlResult resolveArticleImageUrl(const char* baseUrl, const char* src, const char* width, const char* height,
                                      char* out, size_t outCap);

}  // namespace readwise
