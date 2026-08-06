#include "ReadwiseHighlightStore.h"

#include <Memory.h>

#include <cstring>
#include <utility>

namespace readwise {
namespace {

// Record prefix: u16 payloadLen | u8 flags | u32 startByte | u32 endByte |
// u32 bodySize | u16 textLen. Reading it alone lets the scanner skip a
// record's text without loading it.
constexpr size_t RECORD_PREFIX = 2 + HIGHLIGHT_FIXED_PAYLOAD;

uint16_t getU16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (static_cast<uint16_t>(in[1]) << 8)); }

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

}  // namespace

ReadwiseHighlightStore::ReadwiseHighlightStore(ReadwiseFileStore& store, std::string baseDir)
    : store_(store), baseDir_(std::move(baseDir)) {}

std::string ReadwiseHighlightStore::highlightsPath(const char* docId) const {
  return baseDir_ + "/highlights/" + (docId != nullptr ? docId : "") + ".bin";
}

std::string ReadwiseHighlightStore::pendingPath() const { return baseDir_ + "/highlights/pending.bin"; }

bool ReadwiseHighlightStore::scanFile(const std::string& path, size_t& recordCount, size_t& validEnd) {
  recordCount = 0;
  validEnd = 1;

  const long fileSize = store_.size(path);
  if (fileSize < 0) {
    // Absent is the normal no-highlights-yet state.
    return true;
  }
  if (static_cast<size_t>(fileSize) < 1) {
    return true;
  }
  uint8_t version = 0;
  if (store_.readRange(path, 0, &version, 1) != 1 || version != HIGHLIGHTS_FORMAT_VERSION) {
    return false;
  }

  size_t offset = 1;
  while (offset + RECORD_PREFIX <= static_cast<size_t>(fileSize)) {
    uint8_t prefix[RECORD_PREFIX];
    if (store_.readRange(path, offset, prefix, RECORD_PREFIX) != static_cast<int>(RECORD_PREFIX)) {
      break;
    }
    const uint16_t payloadLen = getU16(prefix);
    if (payloadLen < HIGHLIGHT_FIXED_PAYLOAD || payloadLen > HIGHLIGHT_FIXED_PAYLOAD + HIGHLIGHT_TEXT_CAP) {
      // Corrupt length: stop here, everything after it is suspect. The valid
      // prefix of the file remains usable, like a torn journal tail.
      break;
    }
    const size_t recordEnd = offset + 2 + payloadLen;
    if (recordEnd > static_cast<size_t>(fileSize)) {
      // Torn append: the partial record is discarded.
      break;
    }
    recordCount++;
    validEnd = recordEnd;
    offset = recordEnd;
  }
  return true;
}

bool ReadwiseHighlightStore::append(const char* docId, const HighlightRecord& rec) {
  if (!isValidDocumentId(docId)) {
    return false;
  }
  const size_t encoded = encodeHighlight(rec, recordBuffer_, sizeof(recordBuffer_));
  if (encoded == 0) {
    return false;
  }
  if (!store_.ensureDir(baseDir_ + "/highlights")) {
    return false;
  }

  const std::string path = highlightsPath(docId);
  size_t recordCount = 0;
  size_t validEnd = 1;
  if (!scanFile(path, recordCount, validEnd)) {
    return false;
  }
  if (recordCount >= MAX_HIGHLIGHTS_PER_DOC) {
    return false;
  }

  // Register in pending.bin before the record lands: the failure order matters.
  // A pending entry without records is harmlessly cleaned up by the next push;
  // a record without a pending entry would never upload.
  if (!registerPending(docId)) {
    return false;
  }

  // Stream-copy the valid prefix plus the new record (still intact in
  // recordBuffer_; neither scanFile nor registerPending touches it) via the
  // atomic temp+rename path; a crash mid-copy leaves the old file untouched.
  // Reads of `path` during the open write are safe: both store
  // implementations write to a separate temp target until commit.
  if (!store_.beginWrite(path)) {
    return false;
  }
  const uint8_t version = HIGHLIGHTS_FORMAT_VERSION;
  if (!store_.writeChunk(&version, 1)) {
    store_.abortWrite();
    return false;
  }
  size_t offset = 1;
  uint8_t copyBuffer[256];
  while (offset < validEnd) {
    const size_t want = validEnd - offset < sizeof(copyBuffer) ? validEnd - offset : sizeof(copyBuffer);
    const int got = store_.readRange(path, offset, copyBuffer, want);
    if (got != static_cast<int>(want) || !store_.writeChunk(copyBuffer, want)) {
      store_.abortWrite();
      return false;
    }
    offset += want;
  }
  if (!store_.writeChunk(recordBuffer_, encoded) || !store_.commitWrite()) {
    store_.abortWrite();
    return false;
  }
  return true;
}

bool ReadwiseHighlightStore::loadSpans(const char* docId, uint32_t currentBodySize, std::vector<SentenceSpan>& out) {
  if (!isValidDocumentId(docId)) {
    return false;
  }
  const std::string path = highlightsPath(docId);
  const long fileSize = store_.size(path);
  if (fileSize < 0) {
    return true;  // no highlights yet
  }
  uint8_t version = 0;
  if (static_cast<size_t>(fileSize) < 1 || store_.readRange(path, 0, &version, 1) != 1 ||
      version != HIGHLIGHTS_FORMAT_VERSION) {
    return false;
  }
  out.reserve(out.size() + 8);

  size_t offset = 1;
  while (offset + RECORD_PREFIX <= static_cast<size_t>(fileSize) && out.size() < MAX_HIGHLIGHTS_PER_DOC) {
    uint8_t prefix[RECORD_PREFIX];
    if (store_.readRange(path, offset, prefix, RECORD_PREFIX) != static_cast<int>(RECORD_PREFIX)) {
      break;
    }
    const uint16_t payloadLen = getU16(prefix);
    if (payloadLen < HIGHLIGHT_FIXED_PAYLOAD || payloadLen > HIGHLIGHT_FIXED_PAYLOAD + HIGHLIGHT_TEXT_CAP ||
        offset + 2 + payloadLen > static_cast<size_t>(fileSize)) {
      break;
    }
    const uint32_t startByte = getU32(prefix + 3);
    const uint32_t endByte = getU32(prefix + 7);
    const uint32_t bodySize = getU32(prefix + 11);
    if (bodySize == currentBodySize && endByte > startByte) {
      out.push_back({startByte, endByte});
    }
    offset += 2 + payloadLen;
  }
  return true;
}

bool ReadwiseHighlightStore::forEachRecord(const char* docId, void* ctx,
                                           bool (*visit)(void* ctx, const HighlightRecord& rec,
                                                         uint32_t recFileOffset)) {
  if (!isValidDocumentId(docId) || visit == nullptr) {
    return false;
  }
  const std::string path = highlightsPath(docId);
  const long fileSize = store_.size(path);
  if (fileSize < 0) {
    return true;
  }
  uint8_t version = 0;
  if (static_cast<size_t>(fileSize) < 1 || store_.readRange(path, 0, &version, 1) != 1 ||
      version != HIGHLIGHTS_FORMAT_VERSION) {
    return false;
  }

  // ~1 KB, kept off the stack per the 256-byte local guidance.
  auto rec = makeUniqueNoThrow<HighlightRecord>();
  if (!rec) {
    return false;
  }

  bool ok = true;
  size_t offset = 1;
  while (offset + RECORD_PREFIX <= static_cast<size_t>(fileSize)) {
    uint8_t prefix[RECORD_PREFIX];
    if (store_.readRange(path, offset, prefix, RECORD_PREFIX) != static_cast<int>(RECORD_PREFIX)) {
      break;
    }
    const uint16_t payloadLen = getU16(prefix);
    if (payloadLen < HIGHLIGHT_FIXED_PAYLOAD || payloadLen > HIGHLIGHT_FIXED_PAYLOAD + HIGHLIGHT_TEXT_CAP ||
        offset + 2 + payloadLen > static_cast<size_t>(fileSize)) {
      break;
    }
    const size_t total = 2 + payloadLen;
    if (store_.readRange(path, offset, recordBuffer_, total) != static_cast<int>(total) ||
        !decodeHighlight(recordBuffer_, total, *rec)) {
      ok = false;
      break;
    }
    if (!visit(ctx, *rec, static_cast<uint32_t>(offset))) {
      break;
    }
    offset += total;
  }
  return ok;
}

bool ReadwiseHighlightStore::markUploaded(const char* docId, uint32_t recFileOffset) {
  if (!isValidDocumentId(docId)) {
    return false;
  }
  const uint8_t flags = HL_FLAG_UPLOADED;
  return store_.writeRange(highlightsPath(docId), recFileOffset + HIGHLIGHT_FLAGS_OFFSET, &flags, 1);
}

size_t ReadwiseHighlightStore::loadPendingDocIds(std::vector<std::array<char, ID_CAP>>& out) {
  const std::string path = pendingPath();
  const long fileSize = store_.size(path);
  if (fileSize < 0 || static_cast<size_t>(fileSize) < 1) {
    return 0;
  }
  uint8_t version = 0;
  if (store_.readRange(path, 0, &version, 1) != 1 || version != PENDING_FORMAT_VERSION) {
    return 0;
  }
  // Fixed-width entries: a torn append leaves a partial tail that integer
  // division simply excludes.
  const size_t count = (static_cast<size_t>(fileSize) - 1) / ID_CAP;
  out.reserve(out.size() + count);

  size_t loaded = 0;
  for (size_t i = 0; i < count; ++i) {
    std::array<char, ID_CAP> id = {};
    if (store_.readRange(path, 1 + i * ID_CAP, reinterpret_cast<uint8_t*>(id.data()), ID_CAP) !=
        static_cast<int>(ID_CAP)) {
      break;
    }
    id[ID_CAP - 1] = '\0';
    if (!isValidDocumentId(id.data())) {
      continue;
    }
    bool duplicate = false;
    for (const auto& existing : out) {
      if (strncmp(existing.data(), id.data(), ID_CAP) == 0) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      out.push_back(id);
      loaded++;
    }
  }
  return loaded;
}

bool ReadwiseHighlightStore::registerPending(const char* docId) {
  std::vector<std::array<char, ID_CAP>> ids;
  loadPendingDocIds(ids);
  for (const auto& existing : ids) {
    if (strncmp(existing.data(), docId, ID_CAP) == 0) {
      return true;
    }
  }
  std::array<char, ID_CAP> entry = {};
  copyBounded(entry.data(), ID_CAP, docId, strlen(docId));
  ids.push_back(entry);

  // Rewritten whole and atomically, like the journal: the file is tiny (a few
  // dozen 27-byte entries at most).
  std::vector<uint8_t> buffer;
  buffer.reserve(1 + ids.size() * ID_CAP);
  buffer.push_back(PENDING_FORMAT_VERSION);
  for (const auto& id : ids) {
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(id.data()),
                  reinterpret_cast<const uint8_t*>(id.data()) + ID_CAP);
  }
  return store_.writeAll(pendingPath(), buffer.data(), buffer.size());
}

bool ReadwiseHighlightStore::removePendingDocId(const char* docId) {
  if (docId == nullptr) {
    return false;
  }
  std::vector<std::array<char, ID_CAP>> ids;
  loadPendingDocIds(ids);
  std::vector<uint8_t> buffer;
  buffer.reserve(1 + ids.size() * ID_CAP);
  buffer.push_back(PENDING_FORMAT_VERSION);
  bool removed = false;
  for (const auto& id : ids) {
    if (strncmp(id.data(), docId, ID_CAP) == 0) {
      removed = true;
      continue;
    }
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(id.data()),
                  reinterpret_cast<const uint8_t*>(id.data()) + ID_CAP);
  }
  if (!removed) {
    return true;
  }
  return store_.writeAll(pendingPath(), buffer.data(), buffer.size());
}

}  // namespace readwise
