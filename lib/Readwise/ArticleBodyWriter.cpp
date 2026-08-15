#include "ArticleBodyWriter.h"

#include <cstring>
#include <utility>

namespace readwise {
namespace {

ArticleXhtmlWriter::Config makeConfig(const char* sourceUrl, const char* title) {
  ArticleXhtmlWriter::Config config;
  config.baseUrl = sourceUrl;
  config.title = title;
  return config;
}

}  // namespace

ArticleBodyWriter::ArticleBodyWriter(ReadwiseFileStore& store, std::string destPath, const char* sourceUrl,
                                     const char* title)
    : store_(store),
      destPath_(std::move(destPath)),
      writer_(&sEmitBytes, this, &sRecordImage, this, makeConfig(sourceUrl, title)) {}

ArticleBodyWriter::~ArticleBodyWriter() {
  // A writer destroyed mid-body (transfer error, early return) must not leave
  // the store's incremental write open or a temp file behind.
  if (open_ && !committed_) {
    abort();
  }
}

bool ArticleBodyWriter::sEmitBytes(void* ctx, const char* data, size_t len) {
  auto* self = static_cast<ArticleBodyWriter*>(ctx);
  if (!self->ensureOpen()) {
    return false;
  }
  return self->store_.writeChunk(reinterpret_cast<const uint8_t*>(data), len);
}

bool ArticleBodyWriter::sRecordImage(void* ctx, size_t index, const char* url, uint32_t extensionOffset) {
  auto* self = static_cast<ArticleBodyWriter*>(ctx);
  if (index != self->imageCount_ || self->imageCount_ >= MAX_ARTICLE_IMAGES) {
    return true;
  }
  const size_t len = strlen(url);
  if (self->arenaUsed_ + len + 1 > URL_ARENA_CAP) {
    // Out of arena: the reference stays in the XHTML but nothing will be
    // downloaded for it, so it degrades to alt text -- the same outcome as a
    // failed download, and not worth failing the article over.
    return true;
  }
  self->urlStart_[self->imageCount_] = static_cast<uint16_t>(self->arenaUsed_);
  self->extensionOffset_[self->imageCount_] = extensionOffset;
  memcpy(self->urlArena_ + self->arenaUsed_, url, len + 1);
  self->arenaUsed_ += len + 1;
  ++self->imageCount_;
  return true;
}

ArticleImageRef ArticleBodyWriter::image(size_t index) const {
  if (index >= imageCount_) {
    return {"", 0};
  }
  return {urlArena_ + urlStart_[index], extensionOffset_[index]};
}

bool ArticleBodyWriter::ensureOpen() {
  if (open_) {
    return true;
  }
  if (failed_) {
    return false;
  }
  if (!store_.beginWrite(destPath_)) {
    failed_ = true;
    return false;
  }
  open_ = true;
  return true;
}

void ArticleBodyWriter::abort() {
  if (open_) {
    store_.abortWrite();
    open_ = false;
  }
  failed_ = true;
}

bool ArticleBodyWriter::emitPrologueOnce() {
  if (prologueWritten_) {
    return true;
  }
  if (!writer_.begin()) {
    abort();
    return false;
  }
  prologueWritten_ = true;
  return true;
}

bool ArticleBodyWriter::onBodyChunk(const char* data, size_t len) {
  if (failed_) {
    return false;
  }
  // The prologue is emitted lazily on the first chunk so that a body which
  // never arrives leaves no temp file at all.
  if (!emitPrologueOnce()) {
    return false;
  }
  if (!writer_.feed(data, len)) {
    abort();
    return false;
  }
  return true;
}

bool ArticleBodyWriter::onBodyEnd(bool complete) {
  if (failed_) {
    return false;
  }
  if (!complete) {
    // A torn body must never be committed: the reader would present a
    // truncated article as the whole thing.
    abort();
    return false;
  }
  if (!emitPrologueOnce()) {
    return false;
  }
  if (!writer_.finish()) {
    abort();
    return false;
  }
  // An article that produced no body at all still commits -- "fetched, and
  // genuinely empty" and "never fetched" must stay distinguishable.
  if (!ensureOpen()) {
    return false;
  }
  if (!store_.commitWrite()) {
    open_ = false;
    failed_ = true;
    return false;
  }
  open_ = false;
  committed_ = true;
  return true;
}

}  // namespace readwise
