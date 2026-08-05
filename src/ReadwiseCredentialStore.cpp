#include "ReadwiseCredentialStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
// Bumped when a change to defaults would alter behavior for existing configs.
constexpr uint8_t CONFIG_VERSION = 1;

// Matches ReadwiseSyncEngine's DEFAULT_DOCUMENT_CAP. A cap of 0 would mean an
// empty library, so it is clamped rather than trusted.
constexpr uint16_t MIN_DOCUMENT_CAP = 10;
constexpr uint16_t MAX_DOCUMENT_CAP = 500;
constexpr uint16_t DEFAULT_DOCUMENT_CAP = 100;
}  // namespace

void ReadwiseCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  // Obfuscation is device-binding, not encryption; see the header.
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
  doc["documentCap"] = documentCap;
  doc["syncEnabled"] = syncEnabled;
}

bool ReadwiseCredentialStore::fromJson(JsonVariantConst doc) {
  bool needsResave = false;

  const char* obfuscated = doc["token_obf"] | "";
  if (obfuscated[0] != '\0') {
    token = obfuscation::deobfuscateFromBase64(obfuscated);
  } else {
    // A plaintext token from a hand-edited file is accepted once, then rewritten
    // obfuscated on the next save.
    const char* plain = doc["token"] | "";
    token = plain;
    if (token.empty() == false) {
      LOG_DBG("RWS", "Found a plaintext token; it will be obfuscated on next save");
      needsResave = true;
    }
  }

  const uint16_t cap = doc["documentCap"] | DEFAULT_DOCUMENT_CAP;
  if (cap < MIN_DOCUMENT_CAP || cap > MAX_DOCUMENT_CAP) {
    LOG_DBG("RWS", "Invalid documentCap %u in JSON, resetting to %u", cap, DEFAULT_DOCUMENT_CAP);
    documentCap = DEFAULT_DOCUMENT_CAP;
    needsResave = true;
  } else {
    documentCap = cap;
  }

  syncEnabled = doc["syncEnabled"] | false;

  if (needsResave) {
    // saveToFile() here would deadlock on storeMutex; request it instead.
    requestResave();
  }
  return true;
}

void ReadwiseCredentialStore::setToken(const std::string& value) {
  if (value == token) {
    return;
  }
  token = value;
}

void ReadwiseCredentialStore::clearToken() {
  if (token.empty()) {
    return;
  }
  token.clear();
}

void ReadwiseCredentialStore::setDocumentCap(uint16_t cap) {
  if (cap < MIN_DOCUMENT_CAP || cap > MAX_DOCUMENT_CAP || cap == documentCap) {
    return;
  }
  documentCap = cap;
}

void ReadwiseCredentialStore::setSyncEnabled(bool enabled) {
  if (enabled == syncEnabled) {
    return;
  }
  syncEnabled = enabled;
}
