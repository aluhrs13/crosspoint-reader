// The body-to-archive path, and specifically what happens when images fail.
//
// The whole reason image downloads are a second phase is that they must never
// be able to take the article with them. These tests hold that line: 404s,
// oversized transfers, and servers that answer with an HTML error page instead
// of an image all have to leave a readable article behind.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "FakeReadwise.h"
#include "lib/Readwise/ArticleAssembler.h"
#include "lib/Readwise/ArticleBodyWriter.h"

using readwise::ArticleBodyWriter;
using readwise::ArticleImageFetcher;
using readwise::assembleArticleEpub;
using testing_support::FakeFileStore;

namespace {

const std::string kXhtml = "/dir/.body.xhtml";
const std::string kScratch = "/dir/.img.tmp";
const std::string kArchive = "/dir/article.epub";

std::vector<uint8_t> jpegBytes(size_t size = 64) {
  std::vector<uint8_t> data(size, 0x42);
  data[0] = 0xFF;
  data[1] = 0xD8;
  data[2] = 0xFF;
  return data;
}

std::vector<uint8_t> pngBytes(size_t size = 64) {
  std::vector<uint8_t> data(size, 0x42);
  const uint8_t magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  memcpy(data.data(), magic, sizeof(magic));
  return data;
}

// Serves canned payloads by URL; anything unlisted is a failed fetch.
class FakeFetcher : public ArticleImageFetcher {
 public:
  explicit FakeFetcher(FakeFileStore& store) : store_(store) {}

  std::map<std::string, std::vector<uint8_t>> payloads;
  std::vector<std::string> requested;

  bool fetch(const char* url, const std::string& destPath, size_t maxBytes) override {
    requested.push_back(url);
    auto it = payloads.find(url);
    if (it == payloads.end()) {
      return false;
    }
    if (it->second.size() > maxBytes) {
      // Mirrors the real downloader aborting mid-transfer once the cap is hit:
      // nothing usable is left behind.
      return false;
    }
    store_.put(destPath, it->second);
    return true;
  }

 private:
  FakeFileStore& store_;
};

// Runs the body through the writer exactly as the transport would.
ArticleBodyWriter* feedBody(FakeFileStore& store, const std::string& html, const char* baseUrl,
                            std::unique_ptr<ArticleBodyWriter>& holder) {
  holder = std::make_unique<ArticleBodyWriter>(store, kXhtml, baseUrl, "A Title");
  EXPECT_TRUE(holder->onBodyChunk(html.data(), html.size()));
  EXPECT_TRUE(holder->onBodyEnd(true));
  EXPECT_TRUE(holder->committed());
  return holder.get();
}

std::string archiveText(const FakeFileStore& store) {
  const std::vector<uint8_t>& bytes = store.files().at(kArchive);
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool archiveContains(const FakeFileStore& store, const std::string& needle) {
  return archiveText(store).find(needle) != std::string::npos;
}

}  // namespace

TEST(ArticleBodyWriter, CommitsTheXhtmlAndCollectsImageUrls) {
  FakeFileStore store;
  std::unique_ptr<ArticleBodyWriter> holder;
  ArticleBodyWriter* body =
      feedBody(store, "<p>Hi<img src=\"/a.jpg\"><img src=\"b.png\"></p>", "https://ex.com/post/", holder);

  ASSERT_TRUE(store.has(kXhtml));
  EXPECT_EQ(body->imageCount(), 2u);
  EXPECT_STREQ(body->image(0).url, "https://ex.com/a.jpg");
  EXPECT_STREQ(body->image(1).url, "https://ex.com/post/b.png");
}

TEST(ArticleBodyWriter, ATornBodyCommitsNothing) {
  FakeFileStore store;
  ArticleBodyWriter body(store, kXhtml, "https://ex.com/p", "T");
  const std::string html = "<p>half an ar";
  EXPECT_TRUE(body.onBodyChunk(html.data(), html.size()));
  EXPECT_FALSE(body.onBodyEnd(false));

  EXPECT_FALSE(body.committed());
  EXPECT_FALSE(store.has(kXhtml)) << "a truncated article must not look like a cached one";
}

TEST(ArticleBodyWriter, AnEmptyBodyStillCommits) {
  FakeFileStore store;
  std::unique_ptr<ArticleBodyWriter> holder;
  feedBody(store, "", "https://ex.com/p", holder);
  // "Fetched, and genuinely empty" must stay distinguishable from "never
  // fetched", or the sync engine re-downloads it forever.
  EXPECT_TRUE(store.has(kXhtml));
}

TEST(ArticleAssembler, StoresEveryImageAndFoldsInTheArticle) {
  FakeFileStore store;
  std::unique_ptr<ArticleBodyWriter> holder;
  ArticleBodyWriter* body =
      feedBody(store, "<p><img src=\"/a.jpg\"><img src=\"/b.png\"></p>", "https://ex.com/p", holder);

  FakeFetcher fetcher(store);
  fetcher.payloads["https://ex.com/a.jpg"] = jpegBytes();
  fetcher.payloads["https://ex.com/b.png"] = pngBytes();

  const auto result = assembleArticleEpub(store, fetcher, *body, kXhtml, kScratch, kArchive, "T", "A");

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.imagesRequested, 2u);
  EXPECT_EQ(result.imagesStored, 2u);
  ASSERT_TRUE(store.has(kArchive));

  EXPECT_TRUE(archiveContains(store, "mimetype"));
  EXPECT_TRUE(archiveContains(store, "META-INF/container.xml"));
  EXPECT_TRUE(archiveContains(store, "OEBPS/content.opf"));
  EXPECT_TRUE(archiveContains(store, "OEBPS/article.xhtml"));
  EXPECT_TRUE(archiveContains(store, "OEBPS/images/0.jpg"));
  EXPECT_TRUE(archiveContains(store, "OEBPS/images/1.png"));

  // Intermediates must not be left on the card.
  EXPECT_FALSE(store.has(kXhtml));
  EXPECT_FALSE(store.has(kScratch));
}

// The provisional '.jpg' in the XHTML is written before the bytes exist. If
// the patch does not land, the decoder is chosen by extension and picks the
// wrong one (ImageDecoderFactory.cpp:14-40).
TEST(ArticleAssembler, PatchesTheExtensionToMatchTheActualFormat) {
  FakeFileStore store;
  std::unique_ptr<ArticleBodyWriter> holder;
  ArticleBodyWriter* body =
      feedBody(store, "<p><img src=\"/first\"><img src=\"/second\"></p>", "https://ex.com/p", holder);

  FakeFetcher fetcher(store);
  // Neither URL carries an extension, and the formats differ.
  fetcher.payloads["https://ex.com/first"] = pngBytes();
  fetcher.payloads["https://ex.com/second"] = jpegBytes();

  ASSERT_TRUE(assembleArticleEpub(store, fetcher, *body, kXhtml, kScratch, kArchive, "T", "A").ok);

  EXPECT_TRUE(archiveContains(store, "src=\"images/0.png\""));
  EXPECT_TRUE(archiveContains(store, "src=\"images/1.jpg\""));
  EXPECT_TRUE(archiveContains(store, "OEBPS/images/0.png"));
  EXPECT_TRUE(archiveContains(store, "OEBPS/images/1.jpg"));
}

TEST(ArticleAssembler, FailedImagesLeaveTheArticleReadable) {
  FakeFileStore store;
  std::unique_ptr<ArticleBodyWriter> holder;
  ArticleBodyWriter* body = feedBody(store,
                                     "<p>text<img src=\"/gone.jpg\" alt=\"Missing\">"
                                     "<img src=\"/huge.jpg\">"
                                     "<img src=\"/notanimage.jpg\">"
                                     "<img src=\"/fine.jpg\"></p>",
                                     "https://ex.com/p", holder);

  FakeFetcher fetcher(store);
  // /gone.jpg is simply absent -> a 404.
  fetcher.payloads["https://ex.com/huge.jpg"] = jpegBytes(readwise::MAX_IMAGE_BYTES + 1);
  // A server answering with an HTML error page under an image URL.
  const std::string errorPage = "<!doctype html><h1>404</h1>";
  fetcher.payloads["https://ex.com/notanimage.jpg"] = std::vector<uint8_t>(errorPage.begin(), errorPage.end());
  fetcher.payloads["https://ex.com/fine.jpg"] = jpegBytes();

  const auto result = assembleArticleEpub(store, fetcher, *body, kXhtml, kScratch, kArchive, "T", "A");

  EXPECT_TRUE(result.ok) << "no image failure may cost the article";
  EXPECT_EQ(result.imagesRequested, 4u);
  EXPECT_EQ(result.imagesStored, 1u);

  EXPECT_TRUE(archiveContains(store, "text"));
  EXPECT_TRUE(archiveContains(store, "OEBPS/images/3.jpg"));
  // The three failures leave dangling srcs, which the reader turns into
  // "[Image: alt]" rather than an error.
  EXPECT_FALSE(archiveContains(store, "OEBPS/images/0."));
  EXPECT_FALSE(archiveContains(store, "OEBPS/images/1."));
  EXPECT_FALSE(archiveContains(store, "OEBPS/images/2."));
  EXPECT_FALSE(store.has(kScratch)) << "a rejected download must not be left behind";
}

TEST(ArticleAssembler, AnArticleWithNoImagesStillProducesAnArchive) {
  FakeFileStore store;
  std::unique_ptr<ArticleBodyWriter> holder;
  ArticleBodyWriter* body = feedBody(store, "<p>Just words.</p>", "https://ex.com/p", holder);

  FakeFetcher fetcher(store);
  const auto result = assembleArticleEpub(store, fetcher, *body, kXhtml, kScratch, kArchive, "T", "A");

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.imagesRequested, 0u);
  EXPECT_TRUE(fetcher.requested.empty());
  EXPECT_TRUE(archiveContains(store, "Just words."));
}

TEST(ArticleAssembler, StopsFetchingOnceTheTotalBudgetIsSpent) {
  FakeFileStore store;
  std::string html = "<p>";
  for (int i = 0; i < 10; ++i) {
    html += "<img src=\"/i" + std::to_string(i) + ".jpg\">";
  }
  html += "</p>";

  std::unique_ptr<ArticleBodyWriter> holder;
  ArticleBodyWriter* body = feedBody(store, html, "https://ex.com/p", holder);

  FakeFetcher fetcher(store);
  for (int i = 0; i < 10; ++i) {
    fetcher.payloads["https://ex.com/i" + std::to_string(i) + ".jpg"] = jpegBytes(readwise::MAX_IMAGE_BYTES);
  }

  const auto result = assembleArticleEpub(store, fetcher, *body, kXhtml, kScratch, kArchive, "T", "A");

  EXPECT_TRUE(result.ok);
  // 3 MB total against a 512 KB cap each: six fit, and the rest are not even
  // requested rather than being fetched and discarded.
  EXPECT_EQ(result.imagesStored, readwise::MAX_TOTAL_IMAGE_BYTES / readwise::MAX_IMAGE_BYTES);
  EXPECT_LT(fetcher.requested.size(), 10u);
}

TEST(ArticleAssembler, EscapesMetadataIntoTheOpf) {
  FakeFileStore store;
  std::unique_ptr<ArticleBodyWriter> holder;
  ArticleBodyWriter* body = feedBody(store, "<p>x</p>", "https://ex.com/p", holder);

  FakeFetcher fetcher(store);
  // The OPF is parsed by expat too, so an unescaped '&' breaks the package the
  // same way it would break a chapter.
  ASSERT_TRUE(assembleArticleEpub(store, fetcher, *body, kXhtml, kScratch, kArchive, "Tom & Jerry", "A <b> Author").ok);

  EXPECT_TRUE(archiveContains(store, "<dc:title>Tom &amp; Jerry</dc:title>"));
  EXPECT_TRUE(archiveContains(store, "<dc:creator>A &lt;b&gt; Author</dc:creator>"));
}

TEST(ArticleAssembler, AMissingBodyProducesNoArchive) {
  FakeFileStore store;
  ArticleBodyWriter body(store, kXhtml, "https://ex.com/p", "T");
  FakeFetcher fetcher(store);

  const auto result = assembleArticleEpub(store, fetcher, body, kXhtml, kScratch, kArchive, "T", "A");
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(store.has(kArchive));
}
