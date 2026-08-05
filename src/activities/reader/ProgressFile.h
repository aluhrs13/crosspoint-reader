#pragma once

#include <AtomicFile.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ProgressFile {

// Writes `len` bytes of reader progress to `<cachePath>/progress.bin` without
// ever leaving the canonical file half-written.
//
// The crash-safety mechanism and its FAT-level caveats now live in
// atomicfile::writeAtomic; this is the reader-specific path convention on top.
//
// Returns true only if the new progress.bin is fully in place.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  return atomicfile::writeAtomic("PRG", cachePath + "/progress.bin", data, len);
}

}  // namespace ProgressFile
