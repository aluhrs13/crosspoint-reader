#pragma once

#include "ReadwiseApi.h"

// A ReadwiseApi that refuses everything. Offline activities (the library
// browser, managed reading) need a ReadwiseSyncEngine for its storage-side
// operations -- readIndexPage, rebuildLocal, queueing -- and must never touch
// the network; handing them this instead of a real client makes that
// structural rather than a convention.

namespace readwise {

class NullReadwiseApi : public ReadwiseApi {
 public:
  ListResponse fetchPage(const ListQuery&, DocumentSink&) override {
    ListResponse response;
    response.status = ApiStatus::NetworkError;
    return response;
  }
  ApiStatus pushOp(const PendingOp&) override { return ApiStatus::NetworkError; }
  ApiStatus fetchBody(const char*, BodySink&, uint16_t*) override { return ApiStatus::NetworkError; }
};

}  // namespace readwise
