#pragma once

#include <cstddef>
#include <cstdint>

#include "ReadwiseDocument.h"

// A clip posted to the Readwise highlight library (POST /api/v2/highlights/),
// which is what the Obsidian export syncs. It is not a Reader document highlight:
// the same title and source URL still land in their own book.
//
// The quote lives in highlights/<id>.bin, outside the article directory, so
// archiving or deleting the article cannot take an unsent clip with it. Files
// written by older builds at bodies/<id>/highlights.bin are copied across on
// the next read. The journal cannot hold a quote (one-byte payload).
// highlights.idx lists documents that still have an unposted clip, because
// the file store cannot list a directory.

namespace readwise {

inline constexpr const char* HIGHLIGHTS_URL = "https://readwise.io/api/v2/highlights/";
inline constexpr const char* HIGHLIGHT_SOURCE_TYPE = "crosspoint";

// 280 bytes of quote, plus the NUL the in-memory buffer keeps.
inline constexpr size_t HIGHLIGHT_TEXT_MAX = 280;
inline constexpr int HIGHLIGHT_MAX_PER_DOC = 32;
inline constexpr int HIGHLIGHT_INDEX_MAX = 160;

inline constexpr uint8_t HIGHLIGHT_FILE_VERSION = 1;
inline constexpr uint8_t HIGHLIGHT_INDEX_VERSION = 1;
// bit 0: the server accepted the quote. bit 1: the server rejected it (4xx).
// A rejected quote is not retried. The layout is unchanged, so the version stays 1.
inline constexpr uint8_t HIGHLIGHT_FLAG_POSTED = 1;
inline constexpr uint8_t HIGHLIGHT_FLAG_FAILED = 2;

// u8 version | title[TITLE_CAP] | author[AUTHOR_CAP] | sourceUrl[SOURCE_URL_CAP]
inline constexpr size_t HIGHLIGHT_HEADER_BYTES = 1 + TITLE_CAP + AUTHOR_CAP + SOURCE_URL_CAP;
// Header plus every record at the text cap. A larger file is left where it is.
inline constexpr size_t HIGHLIGHT_FILE_MAX_BYTES =
    HIGHLIGHT_HEADER_BYTES + static_cast<size_t>(HIGHLIGHT_MAX_PER_DOC) * (3 + HIGHLIGHT_TEXT_MAX);

// Cuts `text` down to HIGHLIGHT_TEXT_MAX bytes on a UTF-8 boundary, and on a
// word boundary when the cut would drop a partial word. `cap` includes the NUL.
// Returns false when the result is empty.
bool fitHighlightText(char* text, size_t cap);

// One selectable word in reading order. `text` is not necessarily terminated
// at `length`; only `length` bytes are compared.
struct HighlightWordView {
  const char* text = nullptr;
  uint16_t length = 0;
};

enum class HighlightMark : uint8_t { None = 0, Pending = 1, Posted = 2 };

// A cross-page fragment shorter than this is not marked. A full quote is
// marked at any length.
inline constexpr uint16_t HIGHLIGHT_PARTIAL_MIN_WORDS = 3;

// Marks `words` where a stored quote occurs. Quotes are space-separated
// selectable tokens, the same sequence HighlightSelect stores. Every
// non-overlapping full occurrence is marked. A quote with no full match is
// marked only when its prefix is the page's trailing words, or its suffix is
// the page's leading words, and that fragment is at least
// HIGHLIGHT_PARTIAL_MIN_WORDS. Posted wins over pending on the same word.
// `marks` is `wordCount` entries and is cleared first. `quoteFlags` bit 0
// means the server accepted that quote.
void markHighlightWords(const HighlightWordView* words, uint16_t wordCount, const char* const* quotes,
                        const uint8_t* quoteFlags, uint16_t quoteCount, uint8_t* marks);

// {"highlights":[{text, title, author, source_url, source_type, category}]}.
// Empty title, author, and source URL are omitted. Returns false when `out`
// cannot hold the escaped body.
bool buildHighlightBody(const char* text, const char* title, const char* author, const char* sourceUrl, char* out,
                        size_t outCap);

}  // namespace readwise
