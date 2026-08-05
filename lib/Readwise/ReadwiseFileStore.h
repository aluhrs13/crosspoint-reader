#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The seam between the sync engine and the SD card.
//
// This exists so the durability guarantees issue #3 demands -- "interrupted
// writes retain the previous valid data and checkpoint" -- can be tested on the
// host by injecting a write failure at an arbitrary byte, which is not possible
// against a real card.
//
// The SD implementation is SdReadwiseFileStore, which forwards to HalStorage.
// This header deliberately includes no HAL types so the engine stays
// host-compilable.

namespace readwise {

class ReadwiseFileStore {
 public:
  virtual ~ReadwiseFileStore() = default;

  // --- whole-file operations, for the small files -------------------------
  // Replaces `path` atomically (temp + rename). Used for the index, journal,
  // and checkpoint files, all of which are well under a kilobyte.
  virtual bool writeAll(const std::string& path, const uint8_t* data, size_t len) = 0;

  // Reads up to `bufCap` bytes starting at `offset`. Returns the number of bytes
  // read, or -1 on error. A short read at end-of-file is not an error.
  virtual int readRange(const std::string& path, size_t offset, uint8_t* buf, size_t bufCap) = 0;

  // Overwrites `len` bytes at `offset` in an existing file, in place. NOT
  // crash-atomic -- used only for single-byte flag patches (FLAG_HAS_BODY)
  // where a torn write costs at worst a redundant re-download, never
  // structural corruption.
  virtual bool writeRange(const std::string& path, size_t offset, const uint8_t* data, size_t len) = 0;

  virtual bool exists(const std::string& path) = 0;
  virtual bool remove(const std::string& path) = 0;
  // Returns the file size, or -1 if absent or unreadable.
  virtual long size(const std::string& path) = 0;
  virtual bool ensureDir(const std::string& path) = 0;

  // --- incremental write, for docs.bin ------------------------------------
  // docs.bin can reach ~70 KB for a full library, which cannot be assembled in
  // RAM, so it is streamed to a temporary file and renamed into place only once
  // complete. Only one incremental write may be open at a time.
  //
  // A crash before commitWrite leaves the previous docs.bin untouched.
  virtual bool beginWrite(const std::string& path) = 0;
  virtual bool writeChunk(const uint8_t* data, size_t len) = 0;
  // Overwrites bytes already written in the open temp file. Used to patch the
  // docs.bin header once the LUT offset and record count are known, the same
  // seek-back-and-patch shape Section.cpp uses -- and the reason the header is
  // not recomputed by re-reading the whole file, which would cost ~70 KB of RAM.
  virtual bool patchWrite(size_t offset, const uint8_t* data, size_t len) = 0;
  virtual bool commitWrite() = 0;
  virtual void abortWrite() = 0;
};

}  // namespace readwise
