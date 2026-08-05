#include "ReadwiseJournal.h"

#include <cstring>
#include <utility>

namespace readwise {
namespace {

// Bounded so a corrupt length can never drive an unbounded allocation. Well
// above any realistic queue: operations are user actions between syncs.
constexpr size_t MAX_ENTRIES = 512;

}  // namespace

ReadwiseJournal::ReadwiseJournal(ReadwiseFileStore& store, std::string path) : store_(store), path_(std::move(path)) {}

bool ReadwiseJournal::load() {
  entries_.clear();
  nextSeq_ = 1;

  const long fileSize = store_.size(path_);
  if (fileSize < 0) {
    // No journal yet is the normal first-run state, not a failure.
    return true;
  }
  if (static_cast<size_t>(fileSize) < JOURNAL_HEADER_SIZE) {
    return true;
  }

  uint8_t header = 0;
  if (store_.readRange(path_, 0, &header, 1) != 1) {
    return false;
  }
  if (header != JOURNAL_FORMAT_VERSION) {
    return false;
  }

  // A trailing partial entry is discarded rather than treated as corruption:
  // fixed-width entries exist precisely so an interrupted append costs only the
  // record being written.
  const size_t body = static_cast<size_t>(fileSize) - JOURNAL_HEADER_SIZE;
  size_t count = body / JOURNAL_ENTRY_SIZE;
  if (count > MAX_ENTRIES) {
    count = MAX_ENTRIES;
  }
  entries_.reserve(count);

  uint8_t buffer[JOURNAL_ENTRY_SIZE];
  for (size_t i = 0; i < count; ++i) {
    const size_t offset = JOURNAL_HEADER_SIZE + i * JOURNAL_ENTRY_SIZE;
    if (store_.readRange(path_, offset, buffer, JOURNAL_ENTRY_SIZE) != static_cast<int>(JOURNAL_ENTRY_SIZE)) {
      break;
    }
    PendingOp op;
    if (!decodeJournalEntry(buffer, JOURNAL_ENTRY_SIZE, op)) {
      // A malformed entry stops the replay: everything after it is suspect, and
      // dropping the tail is safer than executing a half-understood operation.
      break;
    }
    if (op.seq >= nextSeq_) {
      nextSeq_ = op.seq + 1;
    }
    entries_.push_back(op);
  }
  return true;
}

bool ReadwiseJournal::append(OpType op, const char* id, uint8_t payload, const char* remoteRev) {
  if (id == nullptr || entries_.size() >= MAX_ENTRIES) {
    return false;
  }
  PendingOp entry;
  entry.seq = nextSeq_++;
  copyBounded(entry.id, ID_CAP, id, strlen(id));
  entry.op = op;
  entry.payload = payload;
  if (remoteRev != nullptr) {
    copyBounded(entry.remoteRev, TIMESTAMP_CAP, remoteRev, strlen(remoteRev));
  }
  entries_.push_back(entry);
  return persist();
}

void ReadwiseJournal::coalesce() {
  if (entries_.size() < 2) {
    return;
  }
  std::vector<PendingOp> kept;
  kept.reserve(entries_.size());

  for (size_t i = 0; i < entries_.size(); ++i) {
    const PendingOp& candidate = entries_[i];
    // Keep only the last occurrence of each (document, op) pair. Scanning
    // forward for a later duplicate preserves the relative order of the
    // survivors, so distinct documents are still pushed oldest-first.
    bool superseded = false;
    for (size_t j = i + 1; j < entries_.size(); ++j) {
      if (entries_[j].op == candidate.op && strncmp(entries_[j].id, candidate.id, ID_CAP) == 0) {
        superseded = true;
        break;
      }
    }
    if (!superseded) {
      kept.push_back(candidate);
    }
  }
  entries_.swap(kept);
}

bool ReadwiseJournal::removeAcknowledged(const std::vector<uint32_t>& seqs) {
  if (seqs.empty()) {
    return true;
  }
  std::vector<PendingOp> kept;
  kept.reserve(entries_.size());
  for (const PendingOp& entry : entries_) {
    bool acknowledged = false;
    for (uint32_t seq : seqs) {
      if (entry.seq == seq) {
        acknowledged = true;
        break;
      }
    }
    if (!acknowledged) {
      kept.push_back(entry);
    }
  }
  entries_.swap(kept);
  return persist();
}

const PendingOp* ReadwiseJournal::findLatest(const char* id, OpType op) const {
  if (id == nullptr) {
    return nullptr;
  }
  const PendingOp* latest = nullptr;
  for (const PendingOp& entry : entries_) {
    if (entry.op == op && strncmp(entry.id, id, ID_CAP) == 0) {
      if (latest == nullptr || entry.seq > latest->seq) {
        latest = &entry;
      }
    }
  }
  return latest;
}

bool ReadwiseJournal::persist() {
  // Rewritten whole and atomically rather than appended in place: the file is at
  // most a few kilobytes, and a rewrite is what lets an acknowledged entry be
  // removed from the middle without endangering the entries after it.
  std::vector<uint8_t> buffer;
  buffer.reserve(JOURNAL_HEADER_SIZE + entries_.size() * JOURNAL_ENTRY_SIZE);
  buffer.push_back(JOURNAL_FORMAT_VERSION);

  uint8_t entryBuffer[JOURNAL_ENTRY_SIZE];
  for (const PendingOp& entry : entries_) {
    encodeJournalEntry(entry, entryBuffer);
    buffer.insert(buffer.end(), entryBuffer, entryBuffer + JOURNAL_ENTRY_SIZE);
  }
  return store_.writeAll(path_, buffer.data(), buffer.size());
}

}  // namespace readwise
