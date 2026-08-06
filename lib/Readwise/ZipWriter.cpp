#include "ZipWriter.h"

#include <array>
#include <cstring>

#include "Memory.h"

namespace readwise {
namespace {

constexpr uint32_t LOCAL_HEADER_SIG = 0x04034b50u;
constexpr uint32_t CENTRAL_HEADER_SIG = 0x02014b50u;
constexpr uint32_t EOCD_SIG = 0x06054b50u;

constexpr size_t LOCAL_HEADER_SIZE = 30;
constexpr size_t CENTRAL_HEADER_SIZE = 46;
constexpr size_t EOCD_SIZE = 22;

// Offset of the crc32 field within a local file header. The crc and both sizes
// are only known after the payload has streamed past, so they are patched back
// into the header here rather than buffering the entry.
constexpr size_t LOCAL_CRC_OFFSET = 14;

constexpr uint16_t VERSION_NEEDED = 20;  // 2.0: the floor for a stored entry.
// A fixed 1980-01-01 timestamp. The device has no reliable wall clock at sync
// time, and a stable value keeps archives byte-reproducible for the tests.
constexpr uint16_t DOS_TIME = 0;
constexpr uint16_t DOS_DATE = 0x0021;

constexpr size_t COPY_CHUNK = 1024;

// Standard reflected IEEE 802.3 polynomial, built at compile time so the table
// lands in flash rather than costing DRAM or a startup loop.
constexpr std::array<uint32_t, 256> makeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    table[i] = c;
  }
  return table;
}

constexpr std::array<uint32_t, 256> CRC_TABLE = makeCrcTable();

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

}  // namespace

uint32_t zipCrc32(uint32_t crc, const uint8_t* data, size_t len) {
  uint32_t c = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c = CRC_TABLE[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

ZipWriter::~ZipWriter() {
  if (open_) {
    abort();
  }
}

bool ZipWriter::begin(const std::string& path) {
  if (open_) {
    return false;
  }
  if (!store_.beginWrite(path)) {
    return false;
  }
  entries_.clear();
  entries_.reserve(ZIP_MAX_ENTRIES);
  offset_ = 0;
  entryOpen_ = false;
  failed_ = false;
  open_ = true;
  return true;
}

bool ZipWriter::write(const uint8_t* data, size_t len) {
  if (!store_.writeChunk(data, len)) {
    failed_ = true;
    return false;
  }
  offset_ += static_cast<uint32_t>(len);
  return true;
}

bool ZipWriter::beginEntry(const std::string& name) {
  if (!open_ || failed_ || entryOpen_) {
    return false;
  }
  if (entries_.size() >= ZIP_MAX_ENTRIES || name.empty() || name.size() >= ZIP_NAME_CAP) {
    return false;
  }

  entryNameLen_ = static_cast<uint16_t>(name.size());
  memcpy(entryName_, name.data(), name.size());
  entryName_[name.size()] = '\0';
  entryLocalOffset_ = offset_;
  entryCrc_ = 0;
  entrySize_ = 0;

  uint8_t header[LOCAL_HEADER_SIZE];
  memset(header, 0, sizeof(header));
  put32(header + 0, LOCAL_HEADER_SIG);
  put16(header + 4, VERSION_NEEDED);
  put16(header + 6, 0);  // flags
  put16(header + 8, 0);  // method: stored
  put16(header + 10, DOS_TIME);
  put16(header + 12, DOS_DATE);
  // crc32 (14), compressed size (18), uncompressed size (22) are patched by
  // endEntry once the payload length is known.
  put16(header + 26, entryNameLen_);
  put16(header + 28, 0);  // extra field length

  if (!write(header, sizeof(header))) {
    return false;
  }
  if (!write(reinterpret_cast<const uint8_t*>(entryName_), entryNameLen_)) {
    return false;
  }
  entryOpen_ = true;
  return true;
}

bool ZipWriter::writeData(const uint8_t* data, size_t len) {
  if (!entryOpen_ || failed_) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  entryCrc_ = zipCrc32(entryCrc_, data, len);
  entrySize_ += static_cast<uint32_t>(len);
  return write(data, len);
}

bool ZipWriter::endEntry() {
  if (!entryOpen_ || failed_) {
    return false;
  }
  entryOpen_ = false;

  uint8_t patch[12];
  put32(patch + 0, entryCrc_);
  put32(patch + 4, entrySize_);  // compressed == uncompressed when stored
  put32(patch + 8, entrySize_);
  if (!store_.patchWrite(entryLocalOffset_ + LOCAL_CRC_OFFSET, patch, sizeof(patch))) {
    failed_ = true;
    return false;
  }

  Entry entry{};
  memcpy(entry.name, entryName_, entryNameLen_);
  entry.nameLen = entryNameLen_;
  entry.crc = entryCrc_;
  entry.size = entrySize_;
  entry.localOffset = entryLocalOffset_;
  entries_.push_back(entry);
  return true;
}

bool ZipWriter::addEntry(const std::string& name, const uint8_t* data, size_t len) {
  return beginEntry(name) && writeData(data, len) && endEntry();
}

bool ZipWriter::addEntry(const std::string& name, const std::string& data) {
  return addEntry(name, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

bool ZipWriter::addEntryFromFile(const std::string& name, const std::string& srcPath) {
  const long fileSize = store_.size(srcPath);
  if (fileSize < 0) {
    return false;
  }
  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_CHUNK);
  if (!buffer) {
    return false;
  }
  if (!beginEntry(name)) {
    return false;
  }
  size_t position = 0;
  while (position < static_cast<size_t>(fileSize)) {
    const int read = store_.readRange(srcPath, position, buffer.get(), COPY_CHUNK);
    if (read <= 0) {
      // A short read before the expected size means the source vanished or is
      // unreadable. The entry is already half-written, so the archive is only
      // salvageable by abandoning it.
      failed_ = true;
      return false;
    }
    if (!writeData(buffer.get(), static_cast<size_t>(read))) {
      return false;
    }
    position += static_cast<size_t>(read);
  }
  return endEntry();
}

bool ZipWriter::finish() {
  if (!open_ || failed_ || entryOpen_) {
    abort();
    return false;
  }

  const uint32_t centralDirOffset = offset_;
  for (const Entry& entry : entries_) {
    uint8_t header[CENTRAL_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    put32(header + 0, CENTRAL_HEADER_SIG);
    put16(header + 4, VERSION_NEEDED);  // version made by
    put16(header + 6, VERSION_NEEDED);  // version needed
    put16(header + 8, 0);               // flags
    put16(header + 10, 0);              // method: stored
    put16(header + 12, DOS_TIME);
    put16(header + 14, DOS_DATE);
    put32(header + 16, entry.crc);
    put32(header + 20, entry.size);
    put32(header + 24, entry.size);
    put16(header + 28, entry.nameLen);
    put16(header + 30, 0);  // extra field length
    put16(header + 32, 0);  // comment length
    put16(header + 34, 0);  // disk number start
    put16(header + 36, 0);  // internal attributes
    put32(header + 38, 0);  // external attributes
    put32(header + 42, entry.localOffset);

    if (!write(header, sizeof(header)) || !write(reinterpret_cast<const uint8_t*>(entry.name), entry.nameLen)) {
      abort();
      return false;
    }
  }
  const uint32_t centralDirSize = offset_ - centralDirOffset;

  uint8_t eocd[EOCD_SIZE];
  memset(eocd, 0, sizeof(eocd));
  put32(eocd + 0, EOCD_SIG);
  put16(eocd + 4, 0);  // this disk
  put16(eocd + 6, 0);  // disk with the central directory
  put16(eocd + 8, static_cast<uint16_t>(entries_.size()));
  put16(eocd + 10, static_cast<uint16_t>(entries_.size()));
  put32(eocd + 12, centralDirSize);
  put32(eocd + 16, centralDirOffset);
  put16(eocd + 20, 0);  // comment length

  if (!write(eocd, sizeof(eocd))) {
    abort();
    return false;
  }

  open_ = false;
  return store_.commitWrite();
}

void ZipWriter::abort() {
  if (open_) {
    store_.abortWrite();
  }
  open_ = false;
  entryOpen_ = false;
  entries_.clear();
}

}  // namespace readwise
