#pragma once

#include <HalStorage.h>

#include <string>

#include "ReadwiseFileStore.h"

// The on-device ReadwiseFileStore, backed by HalStorage.
//
// All SD access goes through HalStorage, never raw SdFat: SdFat tracks SPI bus
// state with an unsynchronized flag, and two tasks touching it concurrently can
// trip a FreeRTOS priority-disinherit assert. HalStorage serializes everything
// behind its recursive mutex.

namespace readwise {

class SdReadwiseFileStore : public ReadwiseFileStore {
 public:
  bool writeAll(const std::string& path, const uint8_t* data, size_t len) override;
  int readRange(const std::string& path, size_t offset, uint8_t* buf, size_t bufCap) override;
  bool writeRange(const std::string& path, size_t offset, const uint8_t* data, size_t len) override;
  bool exists(const std::string& path) override;
  bool remove(const std::string& path) override;
  bool removeTree(const std::string& path) override;
  long size(const std::string& path) override;
  bool ensureDir(const std::string& path) override;

  bool beginWrite(const std::string& path) override;
  bool writeChunk(const uint8_t* data, size_t len) override;
  bool patchWrite(size_t offset, const uint8_t* data, size_t len) override;
  bool commitWrite() override;
  void abortWrite() override;

 private:
  // Held open for the duration of an incremental write. Closed before the rename
  // in commitWrite -- SdFat must not rename a path with an open FsFile.
  HalFile pending_;
  std::string pendingFinalPath_;
  std::string pendingTmpPath_;
  bool writeOpen_ = false;
};

}  // namespace readwise
