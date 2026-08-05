#include "BodyTextWriter.h"

#include <utility>

namespace readwise {

BodyTextWriter::BodyTextWriter(ReadwiseFileStore& store, std::string destPath)
    : store_(store), destPath_(std::move(destPath)), extractor_(&sEmitText, this) {}

BodyTextWriter::~BodyTextWriter() {
  // A writer destroyed mid-body (transfer error, early return) must not leave
  // the store's incremental write open or a temp file behind.
  if (open_ && !committed_) {
    abort();
  }
}

bool BodyTextWriter::sEmitText(void* ctx, const char* data, size_t len) {
  auto* self = static_cast<BodyTextWriter*>(ctx);
  if (!self->ensureOpen()) {
    return false;
  }
  return self->store_.writeChunk(reinterpret_cast<const uint8_t*>(data), len);
}

bool BodyTextWriter::ensureOpen() {
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

void BodyTextWriter::abort() {
  if (open_) {
    store_.abortWrite();
    open_ = false;
  }
  failed_ = true;
}

bool BodyTextWriter::onBodyChunk(const char* data, size_t len) {
  if (failed_) {
    return false;
  }
  if (!extractor_.feed(data, len)) {
    abort();
    return false;
  }
  return true;
}

bool BodyTextWriter::onBodyEnd(bool complete) {
  if (failed_) {
    return false;
  }
  if (!complete) {
    // A torn body must never be committed: the reader would present a
    // truncated article as the whole thing.
    abort();
    return false;
  }
  if (!extractor_.finish()) {
    abort();
    return false;
  }
  // An article that produced no text at all still commits an empty file --
  // "fetched, and genuinely empty" and "never fetched" must stay
  // distinguishable.
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
