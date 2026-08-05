#include "ReadwiseListParser.h"

#include <cstdlib>
#include <cstring>

namespace readwise {
namespace {

bool keyIs(const char* key, size_t len, const char* literal) {
  const size_t literalLen = strlen(literal);
  return len == literalLen && memcmp(key, literal, literalLen) == 0;
}

}  // namespace

ReadwiseListParser::ReadwiseListParser(DocumentSink& documents, BodySink* body)
    : documentSink(documents),
      bodySink(body),
      parser(JsonCallbacks{this, &sOnKey, nullptr, &sOnNumber, &sOnBool, &sOnNull, &sOnObjectStart, &sOnObjectEnd,
                           &sOnArrayStart, &sOnArrayEnd, &sOnStringChunk, &sOnStringEnd}) {
  reset();
}

void ReadwiseListParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  skipDepth = 0;
  inBody = false;
  bodyOpen = false;
  aborted = false;
  malformed = false;
  sawCursorKey = false;
  envelopeOpen = false;
  envelopeClosed = false;
  doc.clear();
  fieldLen = 0;
  fieldTruncated = false;
  cursor[0] = '\0';
  totalCount = 0;
  delivered = 0;
}

bool ReadwiseListParser::feed(const char* data, size_t len) {
  if (aborted || malformed) {
    return false;
  }
  parser.feed(data, len);
  return !aborted && !malformed && !parser.hasError();
}

// --- static trampolines ---------------------------------------------------

void ReadwiseListParser::sOnKey(void* ctx, const char* key, size_t len) {
  static_cast<ReadwiseListParser*>(ctx)->onKey(key, len);
}
void ReadwiseListParser::sOnStringChunk(void* ctx, const char* data, size_t len) {
  static_cast<ReadwiseListParser*>(ctx)->onStringChunk(data, len);
}
void ReadwiseListParser::sOnStringEnd(void* ctx) { static_cast<ReadwiseListParser*>(ctx)->onStringEnd(); }
void ReadwiseListParser::sOnNumber(void* ctx, const char* value, size_t len) {
  static_cast<ReadwiseListParser*>(ctx)->onNumber(value, len);
}
void ReadwiseListParser::sOnBool(void* ctx, bool) { static_cast<ReadwiseListParser*>(ctx)->lastKey = LastKey::NONE; }
void ReadwiseListParser::sOnNull(void* ctx) { static_cast<ReadwiseListParser*>(ctx)->onNull(); }
void ReadwiseListParser::sOnObjectStart(void* ctx) { static_cast<ReadwiseListParser*>(ctx)->onObjectStart(); }
void ReadwiseListParser::sOnObjectEnd(void* ctx) { static_cast<ReadwiseListParser*>(ctx)->onObjectEnd(); }
void ReadwiseListParser::sOnArrayStart(void* ctx) { static_cast<ReadwiseListParser*>(ctx)->onArrayStart(); }
void ReadwiseListParser::sOnArrayEnd(void* ctx) { static_cast<ReadwiseListParser*>(ctx)->onArrayEnd(); }

// --- handlers -------------------------------------------------------------

void ReadwiseListParser::onKey(const char* key, size_t len) {
  if (skipDepth > 0) {
    return;
  }
  lastKey = LastKey::NONE;

  if (position == Position::TOP_LEVEL) {
    if (keyIs(key, len, "count")) {
      lastKey = LastKey::COUNT;
    } else if (keyIs(key, len, "nextPageCursor")) {
      lastKey = LastKey::NEXT_PAGE_CURSOR;
      sawCursorKey = true;
    } else if (keyIs(key, len, "results")) {
      lastKey = LastKey::RESULTS;
    }
    return;
  }

  if (position == Position::IN_DOCUMENT) {
    if (keyIs(key, len, "id"))
      lastKey = LastKey::ID;
    else if (keyIs(key, len, "title"))
      lastKey = LastKey::TITLE;
    else if (keyIs(key, len, "author"))
      lastKey = LastKey::AUTHOR;
    else if (keyIs(key, len, "site_name"))
      lastKey = LastKey::SITE_NAME;
    else if (keyIs(key, len, "summary"))
      lastKey = LastKey::SUMMARY;
    else if (keyIs(key, len, "source_url"))
      lastKey = LastKey::SOURCE_URL;
    else if (keyIs(key, len, "updated_at"))
      lastKey = LastKey::UPDATED_AT;
    else if (keyIs(key, len, "last_moved_at"))
      lastKey = LastKey::LAST_MOVED_AT;
    else if (keyIs(key, len, "word_count"))
      lastKey = LastKey::WORD_COUNT;
    else if (keyIs(key, len, "location"))
      lastKey = LastKey::LOCATION;
    else if (keyIs(key, len, "category"))
      lastKey = LastKey::CATEGORY;
    else if (keyIs(key, len, "reading_progress"))
      lastKey = LastKey::READING_PROGRESS;
    else if (keyIs(key, len, "first_opened_at"))
      lastKey = LastKey::FIRST_OPENED_AT;
    else if (keyIs(key, len, "html_content"))
      lastKey = LastKey::HTML_CONTENT;
  }
}

void ReadwiseListParser::onStringChunk(const char* data, size_t len) {
  if (skipDepth > 0) {
    return;
  }
  // html_content bypasses the field buffer entirely: it streams to the body
  // sink as it decodes, which is the whole point of the chunked callbacks.
  if (lastKey == LastKey::HTML_CONTENT && position == Position::IN_DOCUMENT) {
    inBody = true;
    if (bodySink != nullptr) {
      bodyOpen = true;
      if (!bodySink->onBodyChunk(data, len)) {
        aborted = true;
      }
    }
    return;
  }

  const size_t space = FIELD_CAP - 1 - fieldLen;
  const size_t toCopy = len < space ? len : space;
  memcpy(fieldBuf + fieldLen, data, toCopy);
  fieldLen += toCopy;
  if (toCopy < len) {
    // Bounded truncation, not an error: the caps are deliberate (see
    // ReadwiseDocument.h) and a long value must not poison the document.
    fieldTruncated = true;
  }
}

void ReadwiseListParser::onStringEnd() {
  if (skipDepth > 0) {
    return;
  }
  // html_content ends here whether or not any chunk arrived: an empty string
  // is zero chunks followed by the end marker, and the sink must still see
  // onBodyEnd(true) so an empty article commits rather than reading as a
  // failed transfer.
  if (lastKey == LastKey::HTML_CONTENT && position == Position::IN_DOCUMENT) {
    inBody = false;
    bodyOpen = false;
    if (bodySink != nullptr && !bodySink->onBodyEnd(true)) {
      aborted = true;
    }
    lastKey = LastKey::NONE;
    return;
  }
  fieldBuf[fieldLen] = '\0';
  dispatchField();
  fieldLen = 0;
  fieldTruncated = false;
  lastKey = LastKey::NONE;
}

void ReadwiseListParser::dispatchField() {
  if (position == Position::TOP_LEVEL) {
    if (lastKey == LastKey::NEXT_PAGE_CURSOR) {
      copyBounded(cursor, CURSOR_CAP, fieldBuf, fieldLen);
    }
    return;
  }
  if (position != Position::IN_DOCUMENT) {
    return;
  }
  switch (lastKey) {
    case LastKey::ID:
      copyBounded(doc.id, ID_CAP, fieldBuf, fieldLen);
      break;
    case LastKey::TITLE:
      copyBounded(doc.title, TITLE_CAP, fieldBuf, fieldLen);
      break;
    case LastKey::AUTHOR:
      copyBounded(doc.author, AUTHOR_CAP, fieldBuf, fieldLen);
      break;
    case LastKey::SITE_NAME:
      copyBounded(doc.siteName, SITE_NAME_CAP, fieldBuf, fieldLen);
      break;
    case LastKey::SUMMARY:
      copyBounded(doc.summary, SUMMARY_CAP, fieldBuf, fieldLen);
      break;
    case LastKey::SOURCE_URL:
      copyBounded(doc.sourceUrl, SOURCE_URL_CAP, fieldBuf, fieldLen);
      break;
    case LastKey::UPDATED_AT:
      copyBounded(doc.updatedAt, TIMESTAMP_CAP, fieldBuf, fieldLen);
      break;
    case LastKey::LAST_MOVED_AT:
      copyBounded(doc.lastMovedAt, TIMESTAMP_CAP, fieldBuf, fieldLen);
      break;
    case LastKey::FIRST_OPENED_AT:
      // A non-null first_opened_at means the server already considers this
      // document seen; without this, every open would queue a redundant
      // `seen` push. Null leaves the flag clear (onNull skips dispatch).
      if (fieldLen > 0) {
        doc.flags |= FLAG_SEEN;
      }
      break;
    case LastKey::LOCATION:
      doc.location = parseLocation(fieldBuf, fieldLen);
      break;
    case LastKey::CATEGORY:
      doc.category = parseCategory(fieldBuf, fieldLen);
      break;
    default:
      break;
  }
}

void ReadwiseListParser::onNumber(const char* value, size_t len) {
  if (skipDepth > 0) {
    lastKey = LastKey::NONE;
    return;
  }
  if (position == Position::TOP_LEVEL && lastKey == LastKey::COUNT) {
    totalCount = static_cast<uint32_t>(strtoul(value, nullptr, 10));
  } else if (position == Position::IN_DOCUMENT) {
    if (lastKey == LastKey::WORD_COUNT) {
      doc.wordCount = static_cast<uint32_t>(strtoul(value, nullptr, 10));
    } else if (lastKey == LastKey::READING_PROGRESS) {
      // 0..1 fraction on the wire; stored as a 0..100 percent. strtod is fine
      // here -- the value is at most a handful of digits.
      double fraction = strtod(value, nullptr);
      if (fraction < 0.0) fraction = 0.0;
      if (fraction > 1.0) fraction = 1.0;
      doc.readingProgressPercent = static_cast<uint8_t>(fraction * 100.0 + 0.5);
    }
  }
  (void)len;
  lastKey = LastKey::NONE;
}

void ReadwiseListParser::onNull() {
  // Nullable fields keep their cleared defaults; an absent cursor stays empty,
  // which is exactly the "final page" signal.
  lastKey = LastKey::NONE;
}

void ReadwiseListParser::onObjectStart() {
  if (skipDepth > 0) {
    ++skipDepth;
    return;
  }
  switch (position) {
    case Position::TOP_LEVEL:
      if (!envelopeOpen) {
        // The response envelope's own opening brace.
        envelopeOpen = true;
      } else {
        // Any other object at top level belongs to an unknown key: skipped.
        ++skipDepth;
      }
      break;
    case Position::IN_RESULTS:
      position = Position::IN_DOCUMENT;
      doc.clear();
      break;
    case Position::IN_DOCUMENT:
      // tags, or any future nested object: structurally skipped.
      ++skipDepth;
      break;
  }
  lastKey = LastKey::NONE;
}

void ReadwiseListParser::onObjectEnd() {
  if (skipDepth > 0) {
    --skipDepth;
    return;
  }
  if (position == Position::TOP_LEVEL && envelopeOpen) {
    envelopeClosed = true;
    lastKey = LastKey::NONE;
    return;
  }
  if (position == Position::IN_DOCUMENT) {
    // A document without an id is malformed input, not a deliverable record --
    // and an id that is not ULID-shaped must be refused outright, because ids
    // become SD paths (bodies/<id>.txt) and the accepted unverified-TLS
    // posture means response data is not beyond an active attacker.
    if (!isValidDocumentId(doc.id)) {
      malformed = true;
      return;
    }
    ++delivered;
    if (!documentSink.onDocument(doc)) {
      aborted = true;
    }
    position = Position::IN_RESULTS;
  }
  lastKey = LastKey::NONE;
}

void ReadwiseListParser::onArrayStart() {
  if (skipDepth > 0) {
    ++skipDepth;
    return;
  }
  if (position == Position::TOP_LEVEL && lastKey == LastKey::RESULTS) {
    position = Position::IN_RESULTS;
  } else {
    // An array anywhere else (a tags list, an unknown field) is skipped.
    ++skipDepth;
  }
  lastKey = LastKey::NONE;
}

void ReadwiseListParser::onArrayEnd() {
  if (skipDepth > 0) {
    --skipDepth;
    return;
  }
  if (position == Position::IN_RESULTS) {
    position = Position::TOP_LEVEL;
  }
  lastKey = LastKey::NONE;
}

}  // namespace readwise
