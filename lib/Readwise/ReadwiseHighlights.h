#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Pure byte-buffer codec for the on-disk highlight formats plus the sentence
// scanner the selection UI snaps to, documented in docs/file-formats.md.
//
// Like ReadwiseCodec, this depends on nothing but <cstdint>/<cstring> so the
// format layer is host-compilable and unit-testable without HalStorage. All
// multi-byte integers are little-endian and read/written byte-wise -- RISC-V
// faults on unaligned loads.

namespace readwise {

inline constexpr uint8_t HIGHLIGHTS_FORMAT_VERSION = 1;
inline constexpr uint8_t PENDING_FORMAT_VERSION = 1;

// On-device cap on one highlight's text. The Readwise API accepts ~8 KB, but a
// sentence-snapped selection on an 800x480 page is far smaller, and this bound
// sizes the reusable record buffer callers keep as a member.
inline constexpr size_t HIGHLIGHT_TEXT_CAP = 1024;
inline constexpr size_t MAX_HIGHLIGHTS_PER_DOC = 64;

inline constexpr uint8_t HL_FLAG_UPLOADED = 1 << 0;

// --- highlights/<docid>.bin -----------------------------------------------
// File: u8 version, then variable-length records appended one at a time:
//   u16 payloadLen | u8 flags | u32 startByte | u32 endByte | u32 bodySize |
//   u16 textLen | text[textLen]
// payloadLen counts everything after itself, so a loader can skip a record
// without decoding it, and a trailing partial record (payloadLen running past
// EOF) is discarded -- the same torn-append discipline as the journal.
//
// `flags` sits at fixed offset recordStart+2, which is what makes marking a
// record uploaded a single-byte in-place patch (the setBodyCached pattern).
inline constexpr size_t HIGHLIGHT_FIXED_PAYLOAD = 1 + 4 + 4 + 4 + 2;
inline constexpr size_t HIGHLIGHT_FLAGS_OFFSET = 2;  // from record start
inline constexpr size_t MAX_ENCODED_HIGHLIGHT = 2 + HIGHLIGHT_FIXED_PAYLOAD + HIGHLIGHT_TEXT_CAP;

// ~1 KB: heap/member only, never a stack local (same rule as Document).
struct HighlightRecord {
  uint8_t flags = 0;
  // Byte range [startByte, endByte) into bodies/<id>.txt.
  uint32_t startByte = 0;
  uint32_t endByte = 0;
  // Size of the body file when the highlight was captured. A body re-download
  // can shift offsets, so rendering skips records whose bodySize no longer
  // matches; the text snapshot below keeps the upload valid regardless.
  uint32_t bodySize = 0;
  uint16_t textLen = 0;
  char text[HIGHLIGHT_TEXT_CAP + 1] = {};
};

// Returns the number of bytes written, or 0 if the buffer was too small or the
// record's textLen exceeds the cap.
size_t encodeHighlight(const HighlightRecord& rec, uint8_t* out, size_t outCap);
// `consumed` receives the encoded length so a caller walking the file
// sequentially can advance. Returns false on a truncated or corrupt record.
bool decodeHighlight(const uint8_t* in, size_t len, HighlightRecord& rec, size_t* consumed = nullptr);

// --- highlights/pending.bin -----------------------------------------------
// Work list of documents with un-uploaded highlights, so the sync pass never
// has to enumerate the highlights directory (the file-store seam deliberately
// has no directory listing).
//
// u8 version | char docId[ID_CAP] x N -- fixed-width, NUL-padded entries, so a
// torn append is detected and the partial tail discarded, like the journal.

// --- sentence scanner ------------------------------------------------------
// Absolute byte offsets into the body file: [start, end).
struct SentenceSpan {
  uint32_t start = 0;
  uint32_t end = 0;
};

// Splits plain UTF-8 text into sentence spans for the selection UI. A sentence
// ends at a terminator run (. ! ? or U+2026), optionally followed by closing
// quotes/brackets, when the next byte is whitespace or end-of-buffer; any
// newline also ends the current span (the HTML extractor emits newlines only
// between blocks, so this cleanly isolates headings and list items).
// Abbreviation misfires ("Dr. Smith") are accepted -- the UI's extend/shrink
// keys are the escape hatch.
//
// `baseOffset` is the absolute offset of buf[0]; emitted spans are absolute.
// Leading/trailing whitespace is excluded from spans. Scanning stops once
// `maxSpans` spans have been emitted.
void scanSentences(const char* buf, size_t len, uint32_t baseOffset, std::vector<SentenceSpan>& out, size_t maxSpans);

}  // namespace readwise
