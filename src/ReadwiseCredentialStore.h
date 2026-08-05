#pragma once

#include <PersistableStore.h>

#include <string>

/**
 * Singleton storing the Readwise Reader access token on the SD card.
 *
 * The token is XOR-obfuscated with the device's unique hardware MAC address and
 * base64-encoded before writing. That is NOT encryption: it prevents casual
 * reading of the file and ties the stored value to one device, nothing more.
 * Anyone with physical access to the card and knowledge of the scheme can
 * recover the token.
 *
 * Readwise tokens do not expire and have no refresh mechanism, so a revoked
 * token surfaces only as a 401 at request time.
 *
 * Kept in its own file rather than in CrossPointSettings, matching
 * KOReaderCredentialStore -- settings.json is written on ordinary UI
 * interactions, and a credential does not belong in that write path.
 */
class ReadwiseCredentialStore : public PersistableStore<ReadwiseCredentialStore> {
 private:
  std::string token;
  // Documents cached on the device. Bodies are fetched on open rather than
  // prefetched, so this bounds the index rather than transfer volume.
  uint16_t documentCap = 100;
  bool syncEnabled = false;

  ReadwiseCredentialStore() = default;
  ~ReadwiseCredentialStore() = default;

  friend class PersistableStore<ReadwiseCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/readwise.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void setToken(const std::string& value);
  const std::string& getToken() const { return token; }
  bool hasToken() const { return !token.empty(); }
  void clearToken();

  uint16_t getDocumentCap() const { return documentCap; }
  void setDocumentCap(uint16_t cap);

  bool isSyncEnabled() const { return syncEnabled; }
  void setSyncEnabled(bool enabled);

  // The directory holding docs.bin, the indexes, the journal, and the bodies.
  static const char* getDataDir() { return "/.crosspoint/readwise"; }
};

#define READWISE_STORE ReadwiseCredentialStore::getInstance()
