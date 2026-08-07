#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ArticleImageUrl.h"
#include "ArticleXhtmlWriter.h"
#include "ReadwiseApi.h"
#include "ReadwiseFileStore.h"

// BodySink that converts streamed html_content into the XHTML half of an
// article EPUB, and records the images it will need.
//
// The pipeline never holds more than one chunk: JSON-decoded HTML bytes enter
// onBodyChunk, ArticleXhtmlWriter re-emits them a few hundred bytes at a time,
// and each output chunk goes straight to the store's open temp file. The file
// is committed only on a complete body; a torn transfer discards it, so a
// truncated article can never be mistaken for a cached one.
//
// Image URLs are held in a fixed arena rather than a second file because the
// store permits only one incremental write at a time, and because the caller
// (ArticleAssembler) consumes them immediately afterwards in the same scope.
// Nothing here survives the download.

namespace readwise {

struct ArticleImageRef {
  const char* url;
  // Byte offset, within the committed XHTML, of the three provisional
  // extension characters to overwrite once the real format is known.
  uint32_t extensionOffset;
};

class ArticleBodyWriter : public BodySink {
 public:
  // `destPath` receives the XHTML. It is an intermediate: ArticleAssembler
  // folds it into the archive and deletes it.
  ArticleBodyWriter(ReadwiseFileStore& store, std::string destPath, const char* sourceUrl, const char* title);
  ~ArticleBodyWriter() override;

  bool onBodyChunk(const char* data, size_t len) override;
  bool onBodyEnd(bool complete) override;

  bool committed() const { return committed_; }
  bool failed() const { return failed_; }

  size_t imageCount() const { return imageCount_; }
  ArticleImageRef image(size_t index) const;

 private:
  static bool sEmitBytes(void* ctx, const char* data, size_t len);
  static bool sRecordImage(void* ctx, size_t index, const char* url, uint32_t extensionOffset);

  bool ensureOpen();
  void abort();

  // Enough for MAX_ARTICLE_IMAGES URLs of typical length. An image whose URL
  // does not fit is dropped rather than growing the arena: it degrades to alt
  // text, which is the same outcome as a failed download.
  static constexpr size_t URL_ARENA_CAP = 3072;

  // Emits the document prologue on first use; a no-op afterwards.
  bool emitPrologueOnce();

  ReadwiseFileStore& store_;
  std::string destPath_;
  ArticleXhtmlWriter writer_;

  char urlArena_[URL_ARENA_CAP];
  size_t arenaUsed_ = 0;
  uint16_t urlStart_[MAX_ARTICLE_IMAGES] = {};
  uint32_t extensionOffset_[MAX_ARTICLE_IMAGES] = {};
  size_t imageCount_ = 0;

  bool open_ = false;
  // Whether the XHTML prologue has been emitted. Distinct from open_: the
  // writer buffers output, so nothing reaches the store (and open_ stays
  // false) until the first flush. Guarding the prologue on open_ emitted a
  // second one on every body delivered in more than one chunk.
  bool prologueWritten_ = false;
  bool committed_ = false;
  bool failed_ = false;
};

}  // namespace readwise
