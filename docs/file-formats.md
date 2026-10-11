# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8.

## `book.bin`

### Version 10

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 10
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

### Version 50

The header adds `paragraphIndentSpaces` after `extraParagraphSpacing`. The value
participates in cache validation, so sections with different indentation settings
are rebuilt. Version 49 was used by pre-release builds with a different header
layout and is skipped to prevent reuse of those caches.

### Version 48

Version 48 keeps the version 47 serialized layout unchanged. It was bumped
because Hangul text no longer has implicit line-break opportunities between
syllables: Korean words wrap at spaces (like CSS `word-break: keep-all`), and
with hyphenation enabled a word may also split at the end of a line wherever the
CJK line-breaking rules allow, without an inserted hyphen. Justification stretches only word spaces. Cached line breaks and word
positions from version 47 no longer match.

### Version 47

The section header adds signed `characterSpacing` (pixels) and unsigned
`wordSpacingPercent` after `focusReadingEnabled`; both participate in cache
validation. Each TextBlock's BlockStyle stores only `characterSpacing` after
`directionDefined`. Word spacing is resolved into cached word positions during
layout. Sections from earlier versions are rebuilt.

### Version 46

Version 46 keeps the version 45 serialized layout unchanged. It was bumped
because ordered lists now number their items, `list-style-type: none`
suppresses list markers, and `<ul>`/`<ol>` containers contribute their own
margins and padding to child block insets, changing cached word contents and
page layout.

### Version 45

Version 45 keeps the version 44 serialized layout unchanged. It was bumped
because internal EPUB links now preserve CSS superscript and subscript styles,
changing their cached word-style flags and page layout.

### Version 44

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 44 appends the internal-link rectangles produced during text layout to
each serialized page. The reader uses these rectangles for touch navigation;
older caches are rebuilt because they contain no link geometry.

Version 43 keeps the version 42 serialized layout unchanged. It was bumped
because paragraph base direction now excludes direction changes from inline
elements.

Version 42 keeps the version 41 serialized layout unchanged. It was bumped
because closing a block now strips inherited vertical margins and padding.

Version 41 keeps the version 40 serialized layout unchanged. It was bumped
because simple HTML table rows are now laid out as positioned columns rather
than flattened paragraphs with synthetic row/cell labels.

Version 40 keeps the version 39 serialized layout unchanged. It was bumped
because ruby groups now remain intact when large text blocks are soft-flushed.

Version 39 keeps the version 38 serialized layout unchanged. It was bumped
because image top margins are now clamped to keep full-height images within the
page viewport.

Version 38 keeps the version 37 serialized layout unchanged. It was bumped
because Focus Reading now permits line breaks at visible hyphens and dashes
and hyphenates focus-split words as a whole, changing cached page layout.

Version 37 increases the fixed-size footnote href field from 96 to 256 bytes.
This changes each serialized footnote record from 128 to 288 bytes, so older
section caches must be discarded and rebuilt.

Version 36 keeps the version 35 serialized layout unchanged. It was bumped
because ruby and justified text positioning and CJK line breaking now use
corrected word measurements, so version 35 cached page layouts no longer match.

Version 35 adds a header offset and a `uint32_t` entry per page for the
visible-text offset LUT. The other section LUTs remain unchanged.

Version 34 is binary-identical to version 33. The version was bumped because
word-gap suppression was narrowed to tokens glued together in the source: v33
dropped the gap between any two words meeting at a CJK break opportunity, which
collapsed the spaces between Hangul words, so v33 word positions no longer match
what the layout engine now produces.

Version 30 is binary-identical to version 29. The version was bumped because
Arabic contextual shaping changed text measurement (`getTextAdvanceX` now
measures the shaped visual text), so word positions cached by v29 no longer
match what `drawText` renders.

Version 28 introduced serialized word style bits for underline, strikethrough,
superscript, and subscript. The format also includes:

- cache-busting fields for paragraph alignment, hyphenation, embedded CSS,
  image rendering mode, and Focus Reading
- page offset LUT
- per-page visible-text offset LUT (zero-based Unicode codepoints in `<body>`)
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs retained for navigation and legacy sync fallback
- optional per-word Focus Reading split metadata
- per-page footnote entries
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage (v29): per-word arrays plus one shared
  NUL-terminated text blob, replacing v28's length-prefixed word strings. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 50
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 256

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageHorizontalRule = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
    s8 characterSpacing;
};

struct TextBlock {
    u16 wordCount;
    u8 hasFocus;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String srcPath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    u8 paragraphIndentSpaces;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;
    s8 characterSpacing;
    u8 wordSpacingPercent;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;
    u32 visibleTextLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }

    if (visibleTextLutOffset != 0) {
	u32 visibleTextOffset[pageCount] @ visibleTextLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## CLX1 — library index (`.crosspoint/library.idx`)

Written by `lib/LibraryIndex/LibraryBuilder.cpp`, read by `LibraryIndexFile`. One
file describing every book on the card, so the shelf can sort and search
thousands of titles without opening any of them.

Format version 2. An index written by another version fails validation on open
and is rebuilt; that is the entire migration mechanism.

### Layout

| Section | Offset | Contents |
|---|---|---|
| Header | 0 | 64 bytes, `ClixHeader` |
| Folders | `folderStart` | length-prefixed paths, one per folder |
| Records | `recordStart` | `bookCount` × 128-byte `ClixRecord` |
| Permutations | `permStart` | `bookCount` u16 author order, then `bookCount` u16 arrival order |
| Name blob | `nameStart` | per record: path hash, name, canonical author, title, source author (see below) |

The arrival permutation runs oldest first, keyed by the record's FAT
modification time (when the file landed on the card); `firstSeen` — the
build-assigned discovery counter — breaks ties and carries books whose
filesystem reports no time. Fold version 3 introduced the timestamp key; a
fold bump rebuilds ranks while preserving `firstSeen`.
Fold version 4 preserves leading articles in title sort and search keys.

Sections are 512-byte aligned so each starts on an SD block boundary.

### Records are exactly 128 bytes

A fixed stride is what lets the reader seek straight to record *n* without an
offset table, and read a screenful in one 4 KB block. `static_assert` enforces it.

Each record carries `fold[96]`, the title normalised for search and sorting —
accents stripped, case dropped, leading articles preserved — and `authorKey[12]`,
the author's words folded and sorted so that "Victor Hugo" and "Hugo Victor" group as
one person. `authorKey` is a GROUPING key, not an ordering one: the shelf orders by
surname, derived separately from the display name.

The byte before the folded title records metadata extraction status: not
attempted, extracted, or failed. The final four bytes contain the packed FAT
modification date and time returned by SdFat. A zero timestamp is not trusted.
These fields occupy the alignment and reserved bytes from version 1, so the
record remains exactly 128 bytes.

The header records whether EPUB metadata extraction was enabled for the build.
This prevents a metadata-disabled rebuild from making filename fallbacks look
fresh to a later metadata-enabled build.

### The name blob

Per record, at `nameStart + nameOff`:

```text
[u64 pathHash]    FNV-1a fingerprint of the complete path
[nameLen bytes]  filename, without the directory
[u8][author]     display author, one spelling chosen per authorKey across the library
[u8][title]      the book's own title, or length 0 if it never gave one
[u8][source]     cleaned author spelling before the library-wide spelling vote
```

The filename must stay the first textual field and stay the filename: `readPath`
rebuilds a book's path from it, so writing the display title there makes the book
impossible to open. That was a real defect, and it is why title has its own field.

The source author is separate from the displayed canonical author so a later
rebuild can repeat the spelling vote after books are added or removed. Existing
display reads still stop at the author or title fields and retain their offsets.

### Freshness and unchanged rebuilds

Reconciliation treats the persisted 64-bit complete-path fingerprint as the
book identity. Metadata is reused only when the fingerprint, size, nonzero FAT
timestamp, fold version, metadata mode, and expected extraction status agree.
EPUBs with a zero timestamp or a previous extraction failure are parsed again.

If every current record reuses metadata, the old and new counts agree, and no
unreadable entry was seen, the staging files are discarded and the live index is
left byte-for-byte unchanged. A normal rebuild action is therefore a freshness
check, not a forced metadata reread.

### Header flags

`RANKS_DEGRADED` says one or more orders fell back to walk order because a
checked sort allocation failed. Title and author each use a phase-local
`SortKey[bookCount]` allocation (14 bytes per book, 57,344 bytes at the 4,096-book
format ceiling); the first array is released before the second is requested.
Sorting is therefore best effort through the full format limit rather than
being disabled at an arbitrary library size.

`DEDUP_DEGRADED` says a directory exceeded the fixed 1024-entry duplicate-key
buffer, or that its fallible 8 KiB allocation failed. The walk still indexes
every enumerated book; it only stops remembering additional identities for
duplicate-dirent detection, so a damaged FAT may expose duplicates but cannot
make a real book disappear.

`selfSize` is the expected file size. Comparing it against the real one is a free
truncation guard: a build cut short by a power failure cannot pass.

## Readwise cache

The formats below live under `/.crosspoint/readwise/` and back the Readwise
Reader integration. They are independent of the EPUB cache above and share none
of its version numbers.

Unlike the EPUB formats, these do **not** use `Serialization.h`. Multi-byte
integers are read and written byte-wise little-endian by
`lib/Readwise/ReadwiseCodec.cpp`, so the encoding is independent of host
endianness and never performs an unaligned multi-byte load — which RISC-V
faults on. That also keeps the whole format layer host-compilable, so it is
unit-tested on the build machine rather than only on device.

Strings inside a document record are `u16` length-prefixed UTF-8 with no
terminator. Fixed-width string fields elsewhere are NUL-padded to their full
capacity.

Every field capacity is a deliberate truncation point sized from the maxima
observed across 200 real documents; see `docs/readwise-api-contract.md`. Two
truncate in practice: `title` was observed to 318 bytes against a 128-byte
capacity, and `summary` to 2093 bytes against 256.

### `docs.bin` — Version 1

Per-document metadata plus an offset lookup table. Records are written in one
forward pass and the LUT last, the same shape as `book.bin`; the header is
patched with the real LUT offset immediately before the file is renamed into
place, so a crash mid-write leaves the previous `docs.bin` untouched.

Reading one document is two seeks — LUT entry, then record — so a visible page
of the library never loads the rest of the file.

`location` and `category` are persisted **by index**. New values must be
appended immediately before the `Unknown` sentinel (255); renumbering silently
misreads existing caches. `Unknown` exists because the API's location set is
open: `shortlist` occurs on real documents but is absent from Readwise's
published documentation.

`flags` bit 0 is `seen`; bit 1 records that a body has been cached at
`bodies/<id>/article.epub`. Bit 1 is local-only state the server never reports,
so it is carried across a sync rather than taken from the incoming record.

`readingProgressPercent` is 0–100, narrowed from the API's 0–1 fraction. It is
read-only: `PATCH /update/` accepts `reading_progress`, answers `200`, and
silently discards it, so this value is never pushed back.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 1

struct RwString {
    u16 length;
    char value[length];
};

struct RwDocument {
    RwString id;
    RwString title;
    RwString author;
    RwString siteName;
    RwString summary;
    RwString sourceUrl;
    RwString updatedAt;
    RwString lastMovedAt;
    u32 wordCount;
    u8  location;   // 0 new, 1 later, 2 shortlist, 3 archive, 4 feed, 255 unknown
    u8  category;   // 0 article, 1 rss, 2 email, 3 tweet, 4 pdf, 5 video, 6 highlight, 7 note, 8 epub, 255 unknown
    u8  readingProgressPercent;
    u8  flags;      // bit0 seen, bit1 hasBody
};

struct DocsBin {
    u8  version;
    u32 lutOffset;
    u16 recordCount;

    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unexpected docs.bin version {} (expected {})", version, EXPECTED_VERSION));
    }

    RwDocument records[recordCount];

    if ($ != lutOffset) {
        std::warning(std::format("LUT offset mismatch: header says 0x{:X}, records end at 0x{:X}", lutOffset, $));
    }
    u32 lut[recordCount];
};

DocsBin docs @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

### `index_<location>.bin` — Version 1

One file per synced location, currently `index_new.bin` and `index_later.bin`.
Each holds record indexes into `docs.bin`, ordered by `lastMovedAt` descending.
ISO 8601 sorts correctly as a string, so the ordering needs no date parsing.

A visible page of N entries costs `2 * N` bytes plus N record seeks; nothing
outside the page is read. `feed` is deliberately never indexed — it is an RSS
firehose and is the one location large enough to hit the API's 10,000 `count`
cap on its own.

```c++
#define EXPECTED_VERSION 1

struct IndexBin {
    u8  version;
    u16 count;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unexpected index version {} (expected {})", version, EXPECTED_VERSION));
    }
    u16 recordIndex[count];
};

IndexBin index @ 0x00;
```

### `journal.bin` — Version 1

The pending-action queue. Entries are **fixed width**, which is the
append-safety mechanism: a load reads `floor((size - 1) / 66)` entries and
discards a trailing partial record, so an interrupted append costs only the
record being written and never corrupts the operations before it.

`op` is persisted by index and is limited to the two operations the API
demonstrably honours — `0` = set location, `1` = set seen. Reading progress is
never queued, because the update endpoint discards it.

`remoteRev` is the document's `updated_at` when the operation was queued. If it
differs from the incoming value at merge time the document also changed
remotely; the queued local value still wins for its own field, since it
represents a deliberate user action that has not yet been pushed.

```c++
#define EXPECTED_VERSION 1

struct JournalEntry {
    u32  seq;
    char id[27];         // NUL-padded
    u8   op;             // 0 setLocation, 1 setSeen
    u8   payload;        // location value, or 0/1 for seen
    char remoteRev[33];  // NUL-padded ISO 8601
};

struct JournalBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unexpected journal version {} (expected {})", version, EXPECTED_VERSION));
    }
    JournalEntry entries[while(!std::mem::eof())];
};

JournalBin journal @ 0x00;
```

### `checkpoint.bin` — Version 1

The last fully committed sync cursor. Written last and atomically, so it is the
commit point for a sync pass: a failure at any earlier stage leaves the previous
checkpoint in place and the next pass simply re-pulls the same window.

`updatedAfter` is the verbatim highest `updated_at` observed during a completed
sync. It is never synthesized from device time — the device has no reliable RTC
across power cycles, and a cursor slightly in the future loses documents
permanently, whereas one slightly in the past only costs duplicates.

`docCount` is the number of records retained locally. It is not the server's
`count`, which saturates at 10,000 and can never signal completion; only a null
`nextPageCursor` does.

```c++
#define EXPECTED_VERSION 1

struct CheckpointBin {
    u8   version;
    char updatedAfter[33];  // NUL-padded ISO 8601
    u16  docCount;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unexpected checkpoint version {} (expected {})", version, EXPECTED_VERSION));
    }
};

CheckpointBin checkpoint @ 0x00;
```

### `bodies/<id>/`

One directory per article, holding everything the article owns:

```text
bodies/<id>/
    article.epub      store-only (method 0) EPUB, written on device
    epub_<hash>/      the reader's own cache: book.bin, sections, extracted
                      images, .pxc pixel caches, cover/thumb bitmaps

highlights/<id>.bin   quotes taken on this device, version 1
```

`highlights/<id>.bin` is the clip queue for one article. It sits outside
`bodies/<id>/` so archiving or deleting the article does not delete an unsent
quote. Older builds stored the same file at `bodies/<id>/highlights.bin`; the
next read copies it to the new path and leaves the old file in place if that
copy fails. The header snapshots the title, author, and source URL at the
first quote, and each record is a little-endian length, a flags byte, and the
quote bytes (at most 280). `bit 0` is set once `POST /api/v2/highlights/` has
accepted the quote. `bit 1` is set when the server rejects it (HTTP 4xx); that
quote is not retried. A repeat of the same quote is not stored again.
`highlights.idx` at the Readwise root lists the document ids that still have
an unposted, non-rejected quote, because sync cannot list the directory. The
quote file is kept after the index drops the id, so a posted or rejected quote
can still be drawn on the page.

`article.epub` is built from the API's `html_content` as it streams, and
contains `mimetype`, `META-INF/container.xml`, `OEBPS/content.opf`,
`OEBPS/article.xhtml`, and `OEBPS/images/<n>.jpg|png`. Store-only because JPEG
and PNG do not deflate usefully and the firmware has no compressor -- miniz is
built with `MINIZ_NO_DEFLATE_APIS`. There is no header and no version: the file
is either present or absent, and a truncated one is never committed.

The image filenames carry a *provisional* `.jpg` extension when the XHTML is
written, because the local name has to exist before the bytes do. Each is
patched in place to `.png` if magic-byte sniffing says so, which is possible
only because both spellings are three characters. The decoder is selected from
the filename, so this patch is what makes extensionless CDN URLs work.

Putting the reader's cache inside the article directory means eviction is one
recursive delete. Bodies are fetched during sync (or on demand when an
uncached article is opened) and the whole directory is removed when the
document moves to `archive` or `feed`, or disappears from a completed
reconciliation sweep.

Articles synced before this format existed were plain `bodies/<id>.txt` files.
They are swept once on entering the library and re-downloaded.
