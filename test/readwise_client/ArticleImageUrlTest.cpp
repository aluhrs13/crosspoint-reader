// URL resolution and the download-accept policy.
//
// Split out from the XHTML writer because these are the cases real publishers
// actually produce, and they are worth pinning down without a tokenizer in the
// way. The accept rule is deliberately permissive about extensions -- see
// ArticleImageUrl.h -- so the tests here mostly guard the two edges: what must
// resolve correctly, and what must never be fetched.

#include "lib/Readwise/ArticleImageUrl.h"

#include <gtest/gtest.h>

#include <string>

using readwise::ImageUrlResult;
using readwise::resolveArticleImageUrl;

namespace {

constexpr const char* kBase = "https://example.com/blog/2024/post.html?ref=rss";

struct Resolved {
  ImageUrlResult result;
  std::string url;
};

Resolved resolve(const char* src, const char* base = kBase, const char* width = nullptr,
                 const char* height = nullptr) {
  char out[readwise::IMAGE_URL_CAP];
  const ImageUrlResult result = resolveArticleImageUrl(base, src, width, height, out, sizeof(out));
  return {result, std::string(out)};
}

void expectUrl(const char* src, const char* expected) {
  const Resolved resolved = resolve(src);
  EXPECT_EQ(resolved.result, ImageUrlResult::Accepted) << src;
  EXPECT_EQ(resolved.url, expected) << src;
}

void expectRejected(const char* src, ImageUrlResult expected, const char* width = nullptr,
                    const char* height = nullptr) {
  const Resolved resolved = resolve(src, kBase, width, height);
  EXPECT_EQ(resolved.result, expected) << src;
  EXPECT_TRUE(resolved.url.empty()) << "a rejected src must leave no URL behind: " << resolved.url;
}

}  // namespace

TEST(ArticleImageUrl, ResolvesEveryRelativeFormAgainstTheBase) {
  expectUrl("https://cdn.other.net/a.jpg", "https://cdn.other.net/a.jpg");
  expectUrl("//cdn.other.net/a.jpg", "https://cdn.other.net/a.jpg");
  expectUrl("/img/a.jpg", "https://example.com/img/a.jpg");
  expectUrl("a.jpg", "https://example.com/blog/2024/a.jpg");
  expectUrl("./a.jpg", "https://example.com/blog/2024/a.jpg");
  expectUrl("../a.jpg", "https://example.com/blog/a.jpg");
  expectUrl("../../img/a.jpg", "https://example.com/img/a.jpg");
  // Traversal must not climb above the origin.
  expectUrl("../../../../../a.jpg", "https://example.com/a.jpg");
}

TEST(ArticleImageUrl, KeepsQueryStringsAndDropsFragments) {
  // The query is often the entire sizing instruction, so losing it changes the
  // image; the fragment is meaningless to a fetch.
  expectUrl("/a.jpg?w=800&h=600", "https://example.com/a.jpg?w=800&h=600");
  expectUrl("/a.jpg#anchor", "https://example.com/a.jpg");
  expectUrl("/a.jpg?w=800#anchor", "https://example.com/a.jpg?w=800");
}

TEST(ArticleImageUrl, AcceptsUrlsWithNoUsableExtension) {
  // Extensionless CDN URLs are common enough that rejecting them would lose a
  // large share of real article images. The format is settled after download
  // by sniffing magic bytes.
  expectUrl("https://images.unsplash.com/photo-1234?w=800&fm=jpg",
            "https://images.unsplash.com/photo-1234?w=800&fm=jpg");
  expectUrl("/media/abc123", "https://example.com/media/abc123");
  expectUrl("/a.JPG", "https://example.com/a.JPG");
  expectUrl("/a.jpeg", "https://example.com/a.jpeg");
  expectUrl("/a.png", "https://example.com/a.png");
}

TEST(ArticleImageUrl, RejectsWhatCannotOrShouldNotBeFetched) {
  expectRejected("data:image/png;base64,iVBORw0KGgo=", ImageUrlResult::NotHttp);
  expectRejected("javascript:void(0)", ImageUrlResult::NotHttp);
  expectRejected("blob:https://example.com/abc", ImageUrlResult::NotHttp);
  expectRejected("", ImageUrlResult::NoSource);
  expectRejected("   \n  ", ImageUrlResult::NoSource);
  expectRejected(nullptr, ImageUrlResult::NoSource);
}

TEST(ArticleImageUrl, RejectsFormatsWithNoDecoderOnDevice) {
  // Only JPEG and PNG have decoders (ImageDecoderFactory.cpp:14-40); fetching
  // the rest would spend bandwidth and SD space on bytes we then discard.
  for (const char* src : {"/logo.svg", "/anim.gif", "/photo.webp", "/photo.avif", "/scan.tiff",
                          "/icon.ico", "/old.bmp", "/UPPER.SVG"}) {
    expectRejected(src, ImageUrlResult::UnsupportedType);
  }
  // The extension is read from the path, not the query.
  expectRejected("/logo.svg?v=2", ImageUrlResult::UnsupportedType);
  expectUrl("/photo.jpg?fallback=logo.svg", "https://example.com/photo.jpg?fallback=logo.svg");
}

TEST(ArticleImageUrl, RejectsTrackingPixelsByTheirDeclaredSize) {
  expectRejected("/beacon.jpg", ImageUrlResult::TrackingPixel, "1", "1");
  expectRejected("/beacon.jpg", ImageUrlResult::TrackingPixel, "1", nullptr);
  expectRejected("/beacon.jpg", ImageUrlResult::TrackingPixel, nullptr, "2px");
  expectRejected("/beacon.jpg", ImageUrlResult::TrackingPixel, "0", "0");

  // Real images that merely declare a size are content.
  expectUrl("/photo.jpg", "https://example.com/photo.jpg");
  EXPECT_EQ(resolve("/photo.jpg", kBase, "800", "600").result, ImageUrlResult::Accepted);
  EXPECT_EQ(resolve("/photo.jpg", kBase, "100%", nullptr).result, ImageUrlResult::Accepted);
}

TEST(ArticleImageUrl, RelativeSourcesNeedAUsableBase) {
  // source_url is nullable in the API, and a relative src without it cannot be
  // turned into anything fetchable.
  EXPECT_EQ(resolve("a.jpg", nullptr).result, ImageUrlResult::Unresolvable);
  EXPECT_EQ(resolve("/a.jpg", "").result, ImageUrlResult::Unresolvable);
  EXPECT_EQ(resolve("a.jpg", "not a url").result, ImageUrlResult::Unresolvable);
  // An absolute src stands on its own.
  EXPECT_EQ(resolve("https://cdn.net/a.jpg", nullptr).result, ImageUrlResult::Accepted);
  // Protocol-relative falls back to https rather than failing.
  const Resolved inherited = resolve("//cdn.net/a.jpg", nullptr);
  EXPECT_EQ(inherited.result, ImageUrlResult::Accepted);
  EXPECT_EQ(inherited.url, "https://cdn.net/a.jpg");
}

TEST(ArticleImageUrl, TrimsSurroundingWhitespaceFromTheAttribute) {
  // Multi-line src attributes are common in hand-written and templated HTML.
  expectUrl("  \n  /img/a.jpg \t ", "https://example.com/img/a.jpg");
}

TEST(ArticleImageUrl, ReportsRatherThanTruncatesAnOversizedResult) {
  const std::string longSrc = "/" + std::string(readwise::IMAGE_URL_CAP, 'x') + ".jpg";
  char out[readwise::IMAGE_URL_CAP];
  EXPECT_EQ(resolveArticleImageUrl(kBase, longSrc.c_str(), nullptr, nullptr, out, sizeof(out)),
            ImageUrlResult::TooLong);
  EXPECT_STREQ(out, "") << "a truncated URL would fetch the wrong thing";
}

TEST(ArticleImageUrl, ABaseWithNoPathStillResolves) {
  EXPECT_EQ(resolve("a.jpg", "https://example.com").url, "https://example.com/a.jpg");
  EXPECT_EQ(resolve("/a.jpg", "https://example.com").url, "https://example.com/a.jpg");
  EXPECT_EQ(resolve("a.jpg", "https://example.com/").url, "https://example.com/a.jpg");
}
