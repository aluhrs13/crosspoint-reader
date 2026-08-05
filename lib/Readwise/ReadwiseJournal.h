#pragma once

#include <string>
#include <vector>

#include "ReadwiseCodec.h"
#include "ReadwiseFileStore.h"

// The append-safe pending-action journal.
//
// Only operations the API demonstrably honours are ever queued: SetLocation and
// SetSeen. Phase 1 verified that PATCH /update/ answers 200 for
// reading_progress and silently discards it, so reading position is local-only
// state and never enters this queue.

namespace readwise {

class ReadwiseJournal {
 public:
  ReadwiseJournal(ReadwiseFileStore& store, std::string path);

  // Reads the journal, discarding a trailing partial entry. Returns false only
  // on a version mismatch or an unreadable file; an absent journal is an empty
  // journal, not an error.
  bool load();

  // Appends an operation and persists immediately, so a queued action survives
  // a restart even if no sync follows.
  bool append(OpType op, const char* id, uint8_t payload, const char* remoteRev);

  // Collapses repeated operations on the same (document, op) pair down to the
  // newest, preserving the relative order of the survivors so distinct
  // documents are still pushed oldest-first.
  void coalesce();

  // Drops the entries with these sequence numbers and rewrites the file
  // atomically. Used after a push is acknowledged; entries queued later must
  // survive, which is why this is a filtered rewrite rather than a truncation.
  bool removeAcknowledged(const std::vector<uint32_t>& seqs);

  const std::vector<PendingOp>& entries() const { return entries_; }
  bool empty() const { return entries_.empty(); }

  // The most recent queued operation for this document and op type, or nullptr.
  // This is how the merge applies the conflict rule: a queued local value wins
  // for its own field.
  const PendingOp* findLatest(const char* id, OpType op) const;

 private:
  bool persist();

  ReadwiseFileStore& store_;
  std::string path_;
  std::vector<PendingOp> entries_;
  uint32_t nextSeq_ = 1;
};

}  // namespace readwise
