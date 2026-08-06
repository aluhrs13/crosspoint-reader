#include "SdReadwiseFileStore.h"

#include <AtomicFile.h>
#include <Logging.h>

namespace readwise {

bool SdReadwiseFileStore::writeAll(const std::string& path, const uint8_t* data, size_t len) {
  return atomicfile::writeAtomic("RWS", path, data, len);
}

int SdReadwiseFileStore::readRange(const std::string& path, size_t offset, uint8_t* buf, size_t bufCap) {
  HalFile file;
  if (!Storage.openFileForRead("RWS", path, file)) {
    return -1;
  }
  const size_t fileSize = file.size();
  if (offset > fileSize) {
    return -1;
  }
  if (!file.seek(offset)) {
    return -1;
  }
  const size_t available = fileSize - offset;
  const size_t toRead = available < bufCap ? available : bufCap;
  if (toRead == 0) {
    return 0;
  }
  const int read = file.read(buf, toRead);
  // No explicit close: DESTRUCTOR_CLOSES_FILE=1 closes at scope exit.
  return read;
}

bool SdReadwiseFileStore::writeRange(const std::string& path, size_t offset, const uint8_t* data, size_t len) {
  // O_RDWR without O_TRUNC: patch in place. openFileForWrite truncates, so
  // open directly with flags here.
  HalFile file = Storage.open(path.c_str(), O_RDWR);
  if (!file) {
    LOG_ERR("RWS", "writeRange: cannot open %s", path.c_str());
    return false;
  }
  if (!file.seek(offset)) {
    return false;
  }
  const bool ok = file.write(data, len) == len;
  file.flush();
  return ok;
}

bool SdReadwiseFileStore::exists(const std::string& path) { return Storage.exists(path.c_str()); }

bool SdReadwiseFileStore::remove(const std::string& path) { return Storage.remove(path.c_str()); }

bool SdReadwiseFileStore::removeTree(const std::string& path) { return Storage.removeDir(path.c_str()); }

long SdReadwiseFileStore::size(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("RWS", path, file)) {
    return -1;
  }
  return static_cast<long>(file.size());
}

bool SdReadwiseFileStore::ensureDir(const std::string& path) {
  // NOT mkdir: SdFat's mkdir returns false when the directory already exists,
  // which made every sync after the first fail at its opening ensureDir (found
  // on hardware). ensureDirectoryExists is the exists-or-create call.
  return Storage.ensureDirectoryExists(path.c_str());
}

bool SdReadwiseFileStore::beginWrite(const std::string& path) {
  if (writeOpen_) {
    LOG_ERR("RWS", "beginWrite while a write is already open: %s", path.c_str());
    return false;
  }
  pendingFinalPath_ = path;
  pendingTmpPath_ = path + ".tmp";
  if (!Storage.openFileForWrite("RWS", pendingTmpPath_, pending_)) {
    LOG_ERR("RWS", "Could not open %s for write", pendingTmpPath_.c_str());
    return false;
  }
  writeOpen_ = true;
  return true;
}

bool SdReadwiseFileStore::writeChunk(const uint8_t* data, size_t len) {
  if (!writeOpen_) {
    return false;
  }
  return pending_.write(data, len) == len;
}

bool SdReadwiseFileStore::patchWrite(size_t offset, const uint8_t* data, size_t len) {
  if (!writeOpen_) {
    return false;
  }
  // Seek back, overwrite, then return to the end so any later append continues
  // where it left off. Same shape as Section.cpp patching its header offsets.
  const size_t end = pending_.position();
  if (!pending_.seek(offset)) {
    return false;
  }
  const bool ok = pending_.write(data, len) == len;
  if (!pending_.seek(end)) {
    return false;
  }
  return ok;
}

bool SdReadwiseFileStore::commitWrite() {
  if (!writeOpen_) {
    return false;
  }
  writeOpen_ = false;
  pending_.flush();
  // Close before renaming: SdFat must not rename a path that still has an open
  // FsFile.
  pending_.close();

  // SdFat's rename does not overwrite, so the destination goes first. A crash
  // between the two leaves neither file, which reads as "no cache" and triggers
  // a rebuild -- never a torn file.
  Storage.remove(pendingFinalPath_.c_str());
  if (!Storage.rename(pendingTmpPath_.c_str(), pendingFinalPath_.c_str())) {
    LOG_ERR("RWS", "Failed to move %s into place", pendingFinalPath_.c_str());
    Storage.remove(pendingTmpPath_.c_str());
    return false;
  }
  return true;
}

void SdReadwiseFileStore::abortWrite() {
  if (!writeOpen_) {
    return;
  }
  writeOpen_ = false;
  pending_.close();
  Storage.remove(pendingTmpPath_.c_str());
}

}  // namespace readwise
