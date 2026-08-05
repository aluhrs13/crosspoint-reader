#include "ReadwiseSupport.h"

#include <Logging.h>
#include <Memory.h>
#include <NullReadwiseApi.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>

#include "ReadwiseCredentialStore.h"

namespace ReadwiseUi {
namespace {

const char* bodiesPrefix() { return "/.crosspoint/readwise/bodies/"; }

}  // namespace

std::string bodyPathForId(const char* id) {
  // Ids become SD paths; refuse anything not ULID-shaped so a hostile id can
  // never escape the bodies directory. Parsed documents are validated at the
  // parser already -- this guards direct callers.
  if (!readwise::isValidDocumentId(id)) {
    return {};
  }
  return std::string(bodiesPrefix()) + id + ".txt";
}

bool isBodyPath(const std::string& path) { return path.rfind(bodiesPrefix(), 0) == 0; }

std::string idFromBodyPath(const std::string& path) {
  if (!isBodyPath(path)) {
    return {};
  }
  const size_t start = std::string(bodiesPrefix()).size();
  const size_t dot = path.rfind(".txt");
  if (dot == std::string::npos || dot <= start) {
    return {};
  }
  return path.substr(start, dot - start);
}

std::string titleForBodyPath(const std::string& path) {
  const std::string id = idFromBodyPath(path);
  if (id.empty()) {
    return {};
  }
  // Offline lookup only: the null API makes any accidental network use fail
  // fast rather than block. The engine is ~2 KB (scratch Document + record
  // buffer), far over the 256-byte local guidance, so it goes on the heap.
  readwise::NullReadwiseApi nullApi;
  readwise::SdReadwiseFileStore store;
  auto engine = makeUniqueNoThrow<readwise::ReadwiseSyncEngine>(nullApi, store, ReadwiseCredentialStore::getDataDir());
  auto doc = makeUniqueNoThrow<readwise::Document>();
  if (!engine || !doc) {
    LOG_ERR("RWUI", "OOM: title lookup (%s)", !engine ? "engine" : "document");
    return id;
  }
  if (engine->findDocument(id.c_str(), *doc) && doc->title[0] != '\0') {
    return std::string(doc->title);
  }
  return id;
}

StrId statusStrId(readwise::ApiStatus status) {
  switch (status) {
    case readwise::ApiStatus::Ok:
      return StrId::STR_DONE;
    case readwise::ApiStatus::NoCredentials:
      return StrId::STR_READWISE_SET_TOKEN_FIRST;
    case readwise::ApiStatus::AuthFailed:
      return StrId::STR_READWISE_AUTH_FAILED;
    case readwise::ApiStatus::RateLimited:
      return StrId::STR_READWISE_RATE_LIMITED;
    case readwise::ApiStatus::LowMemory:
      return StrId::STR_READWISE_LOW_MEMORY;
    case readwise::ApiStatus::NetworkError:
      return StrId::STR_READWISE_NETWORK_ERROR;
    case readwise::ApiStatus::ParseError:
      return StrId::STR_READWISE_PARSE_ERROR;
    case readwise::ApiStatus::ServerError:
      return StrId::STR_READWISE_SERVER_ERROR;
  }
  return StrId::STR_READWISE_SYNC_FAILED;
}

}  // namespace ReadwiseUi
