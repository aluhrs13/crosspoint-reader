#pragma once

#include <string>

#include "ReadwiseApi.h"
#include "ReadwiseFileStore.h"
#include "ReadwiseHighlightStore.h"
#include "ReadwiseSyncEngine.h"

// Pushes captured highlights to Readwise (POST /api/v2/highlights/) during the
// sync pass. Runs after the metadata sync succeeds and independently of it: a
// failure here leaves every un-acknowledged record pending and the next sync
// simply retries -- the same nothing-is-lost semantics as the journal push.
//
// Records are marked uploaded one flags-patch at a time after a successful
// POST. A crash between the POST and the patch re-uploads at worst one batch;
// the API's dedup behaviour for identical re-posts is pinned by probe 13c.

namespace readwise {

struct HighlightPushOutcome {
  bool ok = false;
  ApiStatus status = ApiStatus::Ok;
  uint16_t uploaded = 0;
  // Records collected but not confirmed uploaded when the pass was abandoned.
  uint16_t failed = 0;
};

// C function pointers per the library callback rule.
struct HighlightSyncHooks {
  void* ctx = nullptr;
  void (*sleepMs)(void* ctx, uint32_t ms) = nullptr;
};

class ReadwiseHighlightSync {
 public:
  ReadwiseHighlightSync(ReadwiseApi& api, ReadwiseSyncEngine& engine, ReadwiseHighlightStore& store);

  HighlightPushOutcome push(const HighlightSyncHooks& hooks);

 private:
  ReadwiseApi& api_;
  ReadwiseSyncEngine& engine_;
  ReadwiseHighlightStore& store_;
};

}  // namespace readwise
