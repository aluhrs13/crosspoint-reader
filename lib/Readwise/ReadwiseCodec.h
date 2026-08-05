#pragma once

#include <cstddef>
#include <cstdint>

#include "ReadwiseDocument.h"

// Pure byte-buffer codecs for the four on-disk Readwise formats, documented in
// docs/file-formats.md.
//
// These deliberately depend on nothing but <cstdint>/<cstring> so the whole
// format layer is host-compilable and unit-testable without HalStorage or a
// filesystem. Everything that touches the SD card lives in ReadwiseFileStore.
//
// Records are encoded one at a time into a caller-supplied buffer rather than
// building a whole file in RAM: 100 documents is roughly 70 KB, which does not
// fit alongside a live TLS session on a device with ~380 KB total.
//
// All multi-byte integers are little-endian and are read and written byte-wise,
// so the format is independent of host endianness and never performs an
// unaligned multi-byte load -- RISC-V faults on those.

namespace readwise {

inline constexpr uint8_t DOCS_FORMAT_VERSION = 1;
inline constexpr uint8_t INDEX_FORMAT_VERSION = 1;
inline constexpr uint8_t JOURNAL_FORMAT_VERSION = 1;
inline constexpr uint8_t CHECKPOINT_FORMAT_VERSION = 1;

// Upper bound on one encoded document record, used to size the reusable buffer
// callers keep as a member.
inline constexpr size_t MAX_ENCODED_RECORD = 1024;

// --- docs.bin header ------------------------------------------------------
// uint8 version | uint32 lutOffset | uint16 recordCount
inline constexpr size_t DOCS_HEADER_SIZE = 1 + 4 + 2;

struct DocsHeader {
  uint8_t version = DOCS_FORMAT_VERSION;
  uint32_t lutOffset = 0;
  uint16_t recordCount = 0;
};

size_t encodeDocsHeader(const DocsHeader& header, uint8_t* out, size_t outCap);
bool decodeDocsHeader(const uint8_t* in, size_t len, DocsHeader& header);

// --- document record ------------------------------------------------------
// Returns the number of bytes written, or 0 if the buffer was too small.
size_t encodeDocument(const Document& doc, uint8_t* out, size_t outCap);
// `consumed` receives the record length so a caller walking a file sequentially
// can advance without consulting the LUT.
bool decodeDocument(const uint8_t* in, size_t len, Document& doc, size_t* consumed = nullptr);

// --- index_<location>.bin -------------------------------------------------
// uint8 version | uint16 count | uint16 recordIndex[count]
inline constexpr size_t INDEX_HEADER_SIZE = 1 + 2;
inline constexpr size_t INDEX_ENTRY_SIZE = 2;

size_t encodeIndexHeader(uint16_t count, uint8_t* out, size_t outCap);
bool decodeIndexHeader(const uint8_t* in, size_t len, uint16_t& count);
void encodeIndexEntry(uint16_t recordIndex, uint8_t* out);
uint16_t decodeIndexEntry(const uint8_t* in);

// --- journal.bin ----------------------------------------------------------
// Operations are limited to the two the API actually honours. Phase 1 proved
// reading_progress is silently discarded by PATCH /update/, so progress is never
// queued for push; it is local-only state.
//
// Persisted by index -- append new values at the end only.
enum class OpType : uint8_t {
  SetLocation = 0,
  SetSeen = 1,
};

// Fixed-width entries are the append-safety mechanism: a load reads
// floor((size - 1) / JOURNAL_ENTRY_SIZE) entries and discards a trailing partial
// record, so an interrupted append can never corrupt earlier operations.
//
// uint32 seq | char id[27] | uint8 op | uint8 payload | char remoteRev[33]
inline constexpr size_t JOURNAL_HEADER_SIZE = 1;
inline constexpr size_t JOURNAL_ENTRY_SIZE = 4 + ID_CAP + 1 + 1 + TIMESTAMP_CAP;

struct PendingOp {
  uint32_t seq = 0;
  char id[ID_CAP] = {};
  OpType op = OpType::SetLocation;
  // For SetLocation, the target Location. For SetSeen, 0 or 1.
  uint8_t payload = 0;
  // The document's updated_at when this op was queued, used to detect that the
  // document changed remotely in the meantime.
  char remoteRev[TIMESTAMP_CAP] = {};
};

void encodeJournalEntry(const PendingOp& op, uint8_t* out);
bool decodeJournalEntry(const uint8_t* in, size_t len, PendingOp& op);

// --- checkpoint.bin -------------------------------------------------------
// uint8 version | char updatedAfter[33] | uint16 docCount
//
// `updatedAfter` is the verbatim highest `updated_at` observed in a fully
// committed sync. It is never synthesized from device time: the device has no
// reliable RTC across power cycles, and a cursor slightly in the future loses
// documents permanently, whereas one slightly in the past only costs duplicates.
inline constexpr size_t CHECKPOINT_SIZE = 1 + TIMESTAMP_CAP + 2;

struct Checkpoint {
  char updatedAfter[TIMESTAMP_CAP] = {};
  uint16_t docCount = 0;
};

size_t encodeCheckpoint(const Checkpoint& checkpoint, uint8_t* out, size_t outCap);
bool decodeCheckpoint(const uint8_t* in, size_t len, Checkpoint& checkpoint);

}  // namespace readwise
