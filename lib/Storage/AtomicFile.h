#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace atomicfile {

// Writes `len` bytes to `finalPath` without ever leaving that file half-written.
//
// The bytes go to `<finalPath>.tmp` first; only once that is fully written and
// closed is it renamed over the destination. An interrupted write (power loss or
// a crash mid-SPI) therefore damages only the throwaway temp file. This was
// originally written for reader progress, where a truncate-in-place write that
// was cut short left the file with a broken FAT cluster chain that the firmware
// could neither rewrite nor clear, stranding the book on an old page (issue
// #2275).
//
// This is crash-safe, not metadata-atomic: on FAT the replace is remove +
// rename, two separate directory operations, so a crash between them can leave
// neither file -- which reads as "absent" on next launch, never a torn file.
// Callers must therefore treat a missing file as a valid state, not an error.
//
// Note: this prevents corruption on a healthy card going forward. It cannot
// repair an already-corrupted file -- removing the stale file may itself fail at
// the FAT level, in which case recovery still requires fsck on a host.
//
// `moduleName` is the 3-letter tag used for logging, matching HalStorage's
// convention.
//
// Returns true only if the new file is fully in place.
inline bool writeAtomic(const char* moduleName, const std::string& finalPath, const uint8_t* data, size_t len) {
  const std::string tmpPath = finalPath + ".tmp";

  {
    HalFile f;
    if (!Storage.openFileForWrite(moduleName, tmpPath, f)) {
      LOG_ERR(moduleName, "Could not open temp file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR(moduleName, "Short write to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written, (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // canonical file first. The brief window where neither file exists reads as
  // "absent" on next launch -- never a corrupt, unclearable file.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR(moduleName, "Failed to rename temp file into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

}  // namespace atomicfile
