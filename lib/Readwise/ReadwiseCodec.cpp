#include "ReadwiseCodec.h"

#include <cstring>

namespace readwise {
namespace {

// Byte-wise little-endian accessors. Writing these out rather than memcpy-ing a
// uint32 keeps the format independent of host endianness, and guarantees no
// unaligned multi-byte load ever happens on RISC-V.
void putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint16_t getU16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (static_cast<uint16_t>(in[1]) << 8)); }

void putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

// Strings are stored as uint16 length + raw bytes, so a record is exactly as
// large as its content. The length is validated against the field capacity on
// read, since a corrupt or hostile length must not overrun the destination.
size_t putString(uint8_t* out, size_t outCap, size_t offset, const char* value) {
  const size_t len = strlen(value);
  if (offset + 2 + len > outCap) {
    return 0;
  }
  putU16(out + offset, static_cast<uint16_t>(len));
  memcpy(out + offset + 2, value, len);
  return offset + 2 + len;
}

// Returns the new offset, or 0 on malformed input.
size_t getString(const uint8_t* in, size_t len, size_t offset, char* dst, size_t dstCap) {
  if (offset + 2 > len) {
    return 0;
  }
  const uint16_t strLen = getU16(in + offset);
  offset += 2;
  if (offset + strLen > len) {
    return 0;
  }
  copyBounded(dst, dstCap, reinterpret_cast<const char*>(in + offset), strLen);
  return offset + strLen;
}

}  // namespace

// --- docs.bin header ------------------------------------------------------

size_t encodeDocsHeader(const DocsHeader& header, uint8_t* out, size_t outCap) {
  if (outCap < DOCS_HEADER_SIZE) {
    return 0;
  }
  out[0] = header.version;
  putU32(out + 1, header.lutOffset);
  putU16(out + 5, header.recordCount);
  return DOCS_HEADER_SIZE;
}

bool decodeDocsHeader(const uint8_t* in, size_t len, DocsHeader& header) {
  if (in == nullptr || len < DOCS_HEADER_SIZE) {
    return false;
  }
  header.version = in[0];
  if (header.version != DOCS_FORMAT_VERSION) {
    return false;
  }
  header.lutOffset = getU32(in + 1);
  header.recordCount = getU16(in + 5);
  return true;
}

// --- document record ------------------------------------------------------

size_t encodeDocument(const Document& doc, uint8_t* out, size_t outCap) {
  if (out == nullptr) {
    return 0;
  }
  size_t offset = 0;
  const char* const strings[] = {doc.id,      doc.title,     doc.author,    doc.siteName,
                                 doc.summary, doc.sourceUrl, doc.updatedAt, doc.lastMovedAt};
  for (const char* value : strings) {
    offset = putString(out, outCap, offset, value);
    if (offset == 0) {
      return 0;
    }
  }
  if (offset + 4 + 4 > outCap) {
    return 0;
  }
  putU32(out + offset, doc.wordCount);
  offset += 4;
  out[offset++] = static_cast<uint8_t>(doc.location);
  out[offset++] = static_cast<uint8_t>(doc.category);
  out[offset++] = doc.readingProgressPercent;
  out[offset++] = doc.flags;
  return offset;
}

bool decodeDocument(const uint8_t* in, size_t len, Document& doc, size_t* consumed) {
  if (in == nullptr) {
    return false;
  }
  doc.clear();
  size_t offset = 0;
  struct Field {
    char* dst;
    size_t cap;
  };
  const Field fields[] = {
      {doc.id, ID_CAP},
      {doc.title, TITLE_CAP},
      {doc.author, AUTHOR_CAP},
      {doc.siteName, SITE_NAME_CAP},
      {doc.summary, SUMMARY_CAP},
      {doc.sourceUrl, SOURCE_URL_CAP},
      {doc.updatedAt, TIMESTAMP_CAP},
      {doc.lastMovedAt, TIMESTAMP_CAP},
  };
  for (const Field& field : fields) {
    offset = getString(in, len, offset, field.dst, field.cap);
    if (offset == 0) {
      return false;
    }
  }
  if (offset + 4 + 4 > len) {
    return false;
  }
  doc.wordCount = getU32(in + offset);
  offset += 4;
  doc.location = static_cast<Location>(in[offset++]);
  doc.category = static_cast<Category>(in[offset++]);
  doc.readingProgressPercent = in[offset++];
  doc.flags = in[offset++];
  if (consumed != nullptr) {
    *consumed = offset;
  }
  return true;
}

// --- index_<location>.bin -------------------------------------------------

size_t encodeIndexHeader(uint16_t count, uint8_t* out, size_t outCap) {
  if (outCap < INDEX_HEADER_SIZE) {
    return 0;
  }
  out[0] = INDEX_FORMAT_VERSION;
  putU16(out + 1, count);
  return INDEX_HEADER_SIZE;
}

bool decodeIndexHeader(const uint8_t* in, size_t len, uint16_t& count) {
  if (in == nullptr || len < INDEX_HEADER_SIZE || in[0] != INDEX_FORMAT_VERSION) {
    return false;
  }
  count = getU16(in + 1);
  return true;
}

void encodeIndexEntry(uint16_t recordIndex, uint8_t* out) { putU16(out, recordIndex); }

uint16_t decodeIndexEntry(const uint8_t* in) { return getU16(in); }

// --- journal.bin ----------------------------------------------------------

void encodeJournalEntry(const PendingOp& op, uint8_t* out) {
  memset(out, 0, JOURNAL_ENTRY_SIZE);
  putU32(out, op.seq);
  size_t offset = 4;
  // Fixed-width, NUL-padded rather than length-prefixed: a constant entry size
  // is what lets a truncated tail be detected and discarded.
  memcpy(out + offset, op.id, strnlen(op.id, ID_CAP - 1));
  offset += ID_CAP;
  out[offset++] = static_cast<uint8_t>(op.op);
  out[offset++] = op.payload;
  memcpy(out + offset, op.remoteRev, strnlen(op.remoteRev, TIMESTAMP_CAP - 1));
}

bool decodeJournalEntry(const uint8_t* in, size_t len, PendingOp& op) {
  if (in == nullptr || len < JOURNAL_ENTRY_SIZE) {
    return false;
  }
  op = PendingOp();
  op.seq = getU32(in);
  size_t offset = 4;
  copyBounded(op.id, ID_CAP, reinterpret_cast<const char*>(in + offset),
              strnlen(reinterpret_cast<const char*>(in + offset), ID_CAP - 1));
  offset += ID_CAP;
  const uint8_t rawOp = in[offset++];
  if (rawOp > static_cast<uint8_t>(OpType::SetSeen)) {
    return false;
  }
  op.op = static_cast<OpType>(rawOp);
  op.payload = in[offset++];
  copyBounded(op.remoteRev, TIMESTAMP_CAP, reinterpret_cast<const char*>(in + offset),
              strnlen(reinterpret_cast<const char*>(in + offset), TIMESTAMP_CAP - 1));
  return true;
}

// --- checkpoint.bin -------------------------------------------------------

size_t encodeCheckpoint(const Checkpoint& checkpoint, uint8_t* out, size_t outCap) {
  if (outCap < CHECKPOINT_SIZE) {
    return 0;
  }
  memset(out, 0, CHECKPOINT_SIZE);
  out[0] = CHECKPOINT_FORMAT_VERSION;
  memcpy(out + 1, checkpoint.updatedAfter, strnlen(checkpoint.updatedAfter, TIMESTAMP_CAP - 1));
  putU16(out + 1 + TIMESTAMP_CAP, checkpoint.docCount);
  return CHECKPOINT_SIZE;
}

bool decodeCheckpoint(const uint8_t* in, size_t len, Checkpoint& checkpoint) {
  if (in == nullptr || len < CHECKPOINT_SIZE || in[0] != CHECKPOINT_FORMAT_VERSION) {
    return false;
  }
  checkpoint = Checkpoint();
  copyBounded(checkpoint.updatedAfter, TIMESTAMP_CAP, reinterpret_cast<const char*>(in + 1),
              strnlen(reinterpret_cast<const char*>(in + 1), TIMESTAMP_CAP - 1));
  checkpoint.docCount = getU16(in + 1 + TIMESTAMP_CAP);
  return true;
}

}  // namespace readwise
