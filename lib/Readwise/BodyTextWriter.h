#pragma once

#include <string>

#include "HtmlTextExtractor.h"
#include "ReadwiseApi.h"
#include "ReadwiseFileStore.h"

// BodySink that converts streamed html_content to plain text and writes it to
// a file through ReadwiseFileStore's incremental-write path.
//
// The pipeline never holds more than one chunk: JSON-decoded HTML bytes enter
// onBodyChunk, HtmlTextExtractor strips them to text a few hundred bytes at a
// time, and each text chunk goes straight to the store's open temp file. The
// file is committed (renamed into place) only on a complete body; an aborted
// or incomplete transfer discards the temp file, so a torn body can never be
// mistaken for a cached article.

namespace readwise {

class BodyTextWriter : public BodySink {
 public:
  BodyTextWriter(ReadwiseFileStore& store, std::string destPath);
  ~BodyTextWriter() override;

  bool onBodyChunk(const char* data, size_t len) override;
  bool onBodyEnd(bool complete) override;

  // True once a body has been fully written and committed.
  bool committed() const { return committed_; }
  bool failed() const { return failed_; }

 private:
  static bool sEmitText(void* ctx, const char* data, size_t len);

  bool ensureOpen();
  void abort();

  ReadwiseFileStore& store_;
  std::string destPath_;
  HtmlTextExtractor extractor_;
  bool open_ = false;
  bool committed_ = false;
  bool failed_ = false;
};

}  // namespace readwise
