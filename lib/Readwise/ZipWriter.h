#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ReadwiseFileStore.h"

// Minimal store-only (method 0) ZIP writer, sufficient to build an EPUB on
// device.
//
// Why store-only: JPEG and PNG payloads do not deflate usefully, and miniz is
// compiled with MINIZ_NO_DEFLATE_APIS / MINIZ_NO_ARCHIVE_WRITING_APIS
// (lib/miniz/src/MinizConfig.h), so no compressor exists in the firmware at
// all. ZipFile's reader treats ZIP_METHOD_STORED as a first-class path
// (ZipFile.cpp:384 and :454) and is in fact cheaper for it -- no 32 KB inflate
// window.
//
// The archive streams through ReadwiseFileStore's incremental write, so the
// destination only appears once finish() commits. A crash mid-assembly leaves
// the previous archive, or nothing.
//
// RAM: one std::vector<Entry> reserved once at begin() (MAX_ENTRIES * 52 B ~=
// 1.6 KB), plus a 1 KB copy buffer allocated per addEntryFromFile call. Payload
// bytes are never held whole -- they stream chunk by chunk to the store.

namespace readwise {

// 4 fixed EPUB members (mimetype, container.xml, content.opf, article.xhtml,
// nav.xhtml) plus the 24-image cap, with headroom.
inline constexpr size_t ZIP_MAX_ENTRIES = 32;
// Longest name we generate is "META-INF/container.xml" (22).
inline constexpr size_t ZIP_NAME_CAP = 40;

uint32_t zipCrc32(uint32_t crc, const uint8_t* data, size_t len);

class ZipWriter {
 public:
  explicit ZipWriter(ReadwiseFileStore& store) : store_(store) {}
  ~ZipWriter();

  ZipWriter(const ZipWriter&) = delete;
  ZipWriter& operator=(const ZipWriter&) = delete;

  // Opens `path` for incremental write. Nothing is visible at `path` until
  // finish() succeeds.
  bool begin(const std::string& path);

  // --- streaming entry ----------------------------------------------------
  bool beginEntry(const std::string& name);
  bool writeData(const uint8_t* data, size_t len);
  bool endEntry();

  // --- convenience --------------------------------------------------------
  bool addEntry(const std::string& name, const uint8_t* data, size_t len);
  bool addEntry(const std::string& name, const std::string& data);
  // Copies an existing file in the store into the archive, 1 KB at a time.
  bool addEntryFromFile(const std::string& name, const std::string& srcPath);

  // Writes the central directory and end-of-central-directory record, then
  // commits. After this the writer is closed either way.
  bool finish();
  void abort();

  size_t entryCount() const { return entries_.size(); }
  bool open() const { return open_; }

 private:
  struct Entry {
    char name[ZIP_NAME_CAP];
    uint16_t nameLen;
    uint32_t crc;
    uint32_t size;
    uint32_t localOffset;
  };

  bool write(const uint8_t* data, size_t len);

  ReadwiseFileStore& store_;
  std::vector<Entry> entries_;
  uint32_t offset_ = 0;
  uint32_t entryCrc_ = 0;
  uint32_t entrySize_ = 0;
  uint32_t entryLocalOffset_ = 0;
  char entryName_[ZIP_NAME_CAP] = {};
  uint16_t entryNameLen_ = 0;
  bool entryOpen_ = false;
  bool open_ = false;
  bool failed_ = false;
};

}  // namespace readwise
