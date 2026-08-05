#include "ReadwiseSupport.h"

#include <Memory.h>
#include <NullReadwiseApi.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>

#include "ReadwiseCredentialStore.h"

namespace ReadwiseUi {
namespace {

const char* bodiesPrefix() { return "/.crosspoint/readwise/bodies/"; }

}  // namespace

std::string bodyPathForId(const char* id) { return std::string(bodiesPrefix()) + (id != nullptr ? id : "") + ".txt"; }

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
    return id;
  }
  if (engine->findDocument(id.c_str(), *doc) && doc->title[0] != '\0') {
    return std::string(doc->title);
  }
  return id;
}

}  // namespace ReadwiseUi
