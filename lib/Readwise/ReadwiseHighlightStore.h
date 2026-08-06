#pragma once

#include <array>
#include <string>
#include <vector>

#include "ReadwiseDocument.h"
#include "ReadwiseFileStore.h"
#include "ReadwiseHighlights.h"

namespace readwise {

// Persistence for captured highlights, on top of the ReadwiseFileStore seam so
// every operation is host-testable against FakeFileStore.
//
// Layout under <baseDir>/highlights/:
//   <docid>.bin  -- variable-length highlight records (see ReadwiseHighlights.h)
//   pending.bin  -- doc ids with un-uploaded highlights, the sync work list
//
// The seam has no in-place append, so append() streams the existing file plus
// the new record through beginWrite/commitWrite: crash-atomic (temp + rename)
// and RAM-bounded to one record buffer regardless of file size.
class ReadwiseHighlightStore {
 public:
  ReadwiseHighlightStore(ReadwiseFileStore& store, std::string baseDir);

  // Persists a record and registers docId in pending.bin. Returns false on I/O
  // failure, an invalid id, or when the per-document cap is reached.
  bool append(const char* docId, const HighlightRecord& rec);

  // Render-time load: spans only, record text is skipped. Records whose
  // bodySize does not match `currentBodySize` are dropped -- their offsets
  // refer to a body file that has since been re-downloaded.
  bool loadSpans(const char* docId, uint32_t currentBodySize, std::vector<SentenceSpan>& out);

  // Walks every record, decoding text. `recFileOffset` is the record's byte
  // offset in the file, usable with markUploaded(). Visitor returns false to
  // stop early. C function pointer per the library callback rule.
  bool forEachRecord(const char* docId, void* ctx,
                     bool (*visit)(void* ctx, const HighlightRecord& rec, uint32_t recFileOffset));

  // Single-byte in-place flags patch (HL_FLAG_UPLOADED), the setBodyCached
  // pattern: a torn write costs at worst one redundant re-upload.
  bool markUploaded(const char* docId, uint32_t recFileOffset);

  // Deduplicated ids from pending.bin. Returns the number loaded.
  size_t loadPendingDocIds(std::vector<std::array<char, ID_CAP>>& out);
  bool removePendingDocId(const char* docId);

  std::string highlightsPath(const char* docId) const;
  std::string pendingPath() const;

 private:
  bool registerPending(const char* docId);
  // Walks record headers; reports the record count and the offset just past
  // the last complete record (torn tails excluded). False on unreadable file;
  // a missing file yields count 0.
  bool scanFile(const std::string& path, size_t& recordCount, size_t& validEnd);

  ReadwiseFileStore& store_;
  std::string baseDir_;
  // Reused for record decode and for the append copy loop; a full record is
  // ~1 KB, which stays off the stack per the 256-byte local guidance.
  uint8_t recordBuffer_[MAX_ENCODED_HIGHLIGHT] = {};
};

}  // namespace readwise
