#pragma once

#include <ArticleAssembler.h>

#include <string>

// Device implementation of the assembler's image fetcher, over HttpDownloader.
//
// Lives in src/ rather than lib/Readwise because it is the one part of the
// article pipeline that needs the network stack; keeping it here is what lets
// the rest stay host-testable.

namespace ReadwiseUi {

class HttpArticleImageFetcher : public readwise::ArticleImageFetcher {
 public:
  bool fetch(const char* url, const std::string& destPath, size_t maxBytes) override;

  // Set from the UI task to abandon an in-flight image.
  void cancel() { cancelled_ = true; }

 private:
  bool cancelled_ = false;
};

}  // namespace ReadwiseUi
