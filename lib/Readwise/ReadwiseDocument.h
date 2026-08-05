#pragma once

#include <cstddef>
#include <cstdint>

namespace readwise {

// Field capacities. Every one is a deliberate truncation point, sized from the
// maxima observed across 200 real documents and recorded in
// docs/readwise-api-contract.md. Two are smaller than the observed maximum and
// will truncate in practice:
//   - title:   observed to 318 bytes
//   - summary: observed to 2093 bytes
// Truncation is preferred to an unbounded record because the whole point of the
// format is a bounded per-document cost; see docs/file-formats.md.
//
// Sizes include the terminating NUL. `id` is a ULID and was exactly 26 bytes in
// every sample, but is treated as bounded rather than fixed in case that changes.
inline constexpr int ID_CAP = 27;
inline constexpr int TITLE_CAP = 129;
inline constexpr int AUTHOR_CAP = 65;
inline constexpr int SITE_NAME_CAP = 49;
inline constexpr int SOURCE_URL_CAP = 161;
inline constexpr int SUMMARY_CAP = 257;
// ISO 8601 with microseconds and offset: "2026-08-05T00:39:04.102135+00:00" is
// 32 bytes, the observed maximum.
inline constexpr int TIMESTAMP_CAP = 33;

// Persisted by index in docs.bin. NEVER renumber or reorder these; append new
// values immediately before the Unknown sentinel, or existing caches are
// silently misread. Same rule as the enums in CrossPointSettings.h.
//
// Unknown exists because the API's location set is open: `shortlist` appeared on
// real documents but is absent from Readwise's published documentation, so a
// client that switches exhaustively on the documented values will mishandle live
// data.
enum class Location : uint8_t {
  New = 0,
  Later = 1,
  Shortlist = 2,
  Archive = 3,
  Feed = 4,
  Unknown = 255,
};

enum class Category : uint8_t {
  Article = 0,
  Rss = 1,
  Email = 2,
  Tweet = 3,
  Pdf = 4,
  Video = 5,
  Highlight = 6,
  Note = 7,
  Epub = 8,
  Unknown = 255,
};

inline constexpr uint8_t FLAG_SEEN = 1 << 0;
// Set when a plain-text body has been fetched and cached under bodies/<id>.txt.
inline constexpr uint8_t FLAG_HAS_BODY = 1 << 1;

// One cached document. ~800 bytes, which is too large for the stack under the
// project's 256-byte local-variable guidance, so callers hold a single instance
// as a member and reuse it across a streaming loop rather than constructing one
// per document.
struct Document {
  char id[ID_CAP] = {};
  char title[TITLE_CAP] = {};
  char author[AUTHOR_CAP] = {};
  char siteName[SITE_NAME_CAP] = {};
  char sourceUrl[SOURCE_URL_CAP] = {};
  char summary[SUMMARY_CAP] = {};
  // The revision used for conflict resolution against a queued operation.
  char updatedAt[TIMESTAMP_CAP] = {};
  // Sort key for the per-location indexes. ISO 8601 orders lexicographically,
  // so no date parsing is needed to sort.
  char lastMovedAt[TIMESTAMP_CAP] = {};

  uint32_t wordCount = 0;
  Location location = Location::Unknown;
  Category category = Category::Unknown;
  // 0..100. The API reports a 0..1 fraction; a percent is ample for a progress
  // bar and keeps the record integral. Read-only: the API silently discards
  // writes to reading_progress, so this is never pushed back.
  uint8_t readingProgressPercent = 0;
  uint8_t flags = 0;

  void clear() { *this = Document(); }
};

// Parses the wire values into the enums above, mapping anything unrecognized to
// Unknown rather than guessing.
Location parseLocation(const char* value, size_t len);
Category parseCategory(const char* value, size_t len);
const char* locationName(Location location);

// Copies `srcLen` bytes of `src` into `dst`, truncating to fit and always
// NUL-terminating. Every string field is written through this.
void copyBounded(char* dst, size_t dstCap, const char* src, size_t srcLen);

// True for an id safe to embed in an SD path (bodies/<id>.txt). Ids observed
// on the wire are 26-character lowercase ULIDs; anything outside [0-9a-z] or
// an implausible length is refused, because ids arrive over an unverified TLS
// session and a crafted "../" id could otherwise escape the bodies directory.
bool isValidDocumentId(const char* id);

}  // namespace readwise
