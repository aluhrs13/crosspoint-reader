#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ArticleBodyWriter.h"
#include "ReadwiseFileStore.h"

// Turns a converted article body plus its images into a store-only EPUB.
//
// This runs after fetchBody has returned, not during it: the body arrives
// inside a TLS read callback, where a nested HTTP request is impossible, and
// where peak heap is already at its worst. Keeping the downloads in a second
// phase also means a total image failure still yields a fully readable
// article, because the XHTML is already on disk by then.
//
// Every per-image failure is non-fatal. A missing archive entry leaves a
// dangling src, which ChapterHtmlSlimParser renders as "[Image: alt]"
// (ChapterHtmlSlimParser.cpp:601, :843) -- the reader's own fallback.
//
// The archive is written with the XHTML last, because the provisional '.jpg'
// extensions inside it are patched as each image is sniffed, and a zip entry
// cannot be edited once written.

namespace readwise {

// Per-image transfer caps. A single article should not be able to spend the
// whole sync budget or fill the card.
inline constexpr size_t MAX_IMAGE_BYTES = 512u * 1024u;
inline constexpr size_t MAX_TOTAL_IMAGE_BYTES = 3u * 1024u * 1024u;

// Injected so the assembler stays host-testable; the device implementation
// wraps HttpDownloader::downloadToFile.
class ArticleImageFetcher {
 public:
  virtual ~ArticleImageFetcher() = default;
  // Fetches `url` into `destPath`, refusing anything beyond `maxBytes`.
  // Returns false on any failure -- the caller treats that as "no image".
  virtual bool fetch(const char* url, const std::string& destPath, size_t maxBytes) = 0;
};

// Stands in when no fetcher is available -- an offline assembly, or a caller
// that has not wired one up. Every image degrades to alt text, which is the
// same path a 404 takes, so the article is still complete and readable.
class NullArticleImageFetcher : public ArticleImageFetcher {
 public:
  bool fetch(const char*, const std::string&, size_t) override { return false; }
};

// Reports progress so the sync popup can say which image it is on. Optional.
using ArticleImageProgress = void (*)(void* ctx, size_t done, size_t total);

struct ArticleAssemblyResult {
  bool ok = false;
  size_t imagesRequested = 0;
  size_t imagesStored = 0;
};

// `xhtmlPath` is consumed: on success it is folded into `archivePath` and
// removed. `title` and `author` populate the OPF metadata.
ArticleAssemblyResult assembleArticleEpub(ReadwiseFileStore& store, ArticleImageFetcher& fetcher,
                                          const ArticleBodyWriter& body, const std::string& xhtmlPath,
                                          const std::string& scratchPath, const std::string& archivePath,
                                          const char* title, const char* author,
                                          ArticleImageProgress progress = nullptr, void* progressCtx = nullptr);

}  // namespace readwise
