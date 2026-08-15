#include "ReadwiseImageFetcher.h"

#include <HalStorage.h>
#include <Logging.h>

#include "network/HttpDownloader.h"

namespace ReadwiseUi {

bool HttpArticleImageFetcher::fetch(const char* url, const std::string& destPath, size_t maxBytes) {
  if (url == nullptr || url[0] == '\0' || cancelled_) {
    return false;
  }

  // The size cap is enforced from the progress callback rather than up front:
  // Content-Length is frequently absent or wrong on image CDNs, so the only
  // reliable limit is on bytes actually received.
  bool overLimit = false;
  bool abortFlag = cancelled_;
  const HttpDownloader::DownloadError error = HttpDownloader::downloadToFile(
      url, destPath,
      [&](size_t downloaded, size_t) {
        if (downloaded > maxBytes) {
          overLimit = true;
          abortFlag = true;
        }
      },
      &abortFlag);

  if (error != HttpDownloader::OK || overLimit) {
    if (overLimit) {
      LOG_DBG("RWIMG", "Image over %u bytes, skipped: %s", static_cast<unsigned>(maxBytes), url);
    } else {
      LOG_DBG("RWIMG", "Image fetch failed (%d): %s", static_cast<int>(error), url);
    }
    // An aborted transfer leaves a partial file, which would sniff as garbage
    // or, worse, as a valid truncated JPEG.
    Storage.remove(destPath.c_str());
    return false;
  }
  return true;
}

}  // namespace ReadwiseUi
