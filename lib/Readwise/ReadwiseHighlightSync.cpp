#include "ReadwiseHighlightSync.h"

#include <Memory.h>

#include <cstring>
#include <vector>

namespace readwise {
namespace {

// One POST's worth of highlight text, copied out of the store's reusable
// record buffer. 8 KB transient heap during the sync pass only, which already
// requires 35 KB free for TLS.
constexpr size_t BATCH_TEXT_CAP = 8 * 1024;
constexpr size_t BATCH_MAX_ITEMS = 16;

// Mirrors the sync engine's rate-limit posture: one guided retry, capped so a
// hostile retry-after cannot park the watchdog-supervised loop.
constexpr uint32_t RATE_LIMIT_WAIT_CAP_MS = 30000;
constexpr uint32_t DEFAULT_RATE_LIMIT_WAIT_MS = 20000;

struct BatchItem {
  uint32_t recFileOffset = 0;
  size_t textPos = 0;  // offset into the batch text buffer
};

struct CollectCtx {
  char* textBuf = nullptr;
  size_t textUsed = 0;
  BatchItem items[BATCH_MAX_ITEMS];
  size_t itemCount = 0;
};

// Copies un-uploaded records into the batch until it is full. Restarting the
// walk after a flush naturally skips what was just marked uploaded.
bool collectVisitor(void* ctxRaw, const HighlightRecord& rec, uint32_t recFileOffset) {
  auto* ctx = static_cast<CollectCtx*>(ctxRaw);
  if ((rec.flags & HL_FLAG_UPLOADED) != 0 || rec.textLen == 0) {
    return true;
  }
  if (ctx->itemCount >= BATCH_MAX_ITEMS || ctx->textUsed + rec.textLen + 1 > BATCH_TEXT_CAP) {
    return false;  // batch full; stop the walk, flush, come back
  }
  BatchItem& item = ctx->items[ctx->itemCount++];
  item.recFileOffset = recFileOffset;
  item.textPos = ctx->textUsed;
  memcpy(ctx->textBuf + ctx->textUsed, rec.text, rec.textLen);
  ctx->textBuf[ctx->textUsed + rec.textLen] = '\0';
  ctx->textUsed += rec.textLen + 1;
  return true;
}

}  // namespace

ReadwiseHighlightSync::ReadwiseHighlightSync(ReadwiseApi& api, ReadwiseSyncEngine& engine,
                                             ReadwiseHighlightStore& store)
    : api_(api), engine_(engine), store_(store) {}

HighlightPushOutcome ReadwiseHighlightSync::push(const HighlightSyncHooks& hooks) {
  HighlightPushOutcome outcome;

  std::vector<std::array<char, ID_CAP>> pending;
  store_.loadPendingDocIds(pending);
  if (pending.empty()) {
    outcome.ok = true;
    return outcome;
  }

  auto textBuf = makeUniqueNoThrow<char[]>(BATCH_TEXT_CAP);
  // ~800 B and ~1 KB respectively: heap, never stack.
  auto doc = makeUniqueNoThrow<Document>();
  auto ctx = makeUniqueNoThrow<CollectCtx>();
  if (!textBuf || !doc || !ctx) {
    outcome.status = ApiStatus::LowMemory;
    return outcome;
  }

  bool rateLimitRetryAvailable = true;

  for (const auto& id : pending) {
    // Title and source_url attach the highlight to the right Reader document.
    // A document evicted from the local cache still uploads text-only.
    const char* title = "";
    const char* sourceUrl = "";
    if (engine_.findDocument(id.data(), *doc)) {
      title = doc->title;
      sourceUrl = doc->sourceUrl;
    }

    while (true) {
      ctx->textBuf = textBuf.get();
      ctx->textUsed = 0;
      ctx->itemCount = 0;
      if (!store_.forEachRecord(id.data(), ctx.get(), collectVisitor)) {
        // Unreadable highlight file: skip the document, keep it pending.
        outcome.failed++;
        break;
      }
      if (ctx->itemCount == 0) {
        // Everything for this document is uploaded.
        store_.removePendingDocId(id.data());
        break;
      }

      HighlightPayload payloads[BATCH_MAX_ITEMS];
      for (size_t i = 0; i < ctx->itemCount; ++i) {
        payloads[i].text = ctx->textBuf + ctx->items[i].textPos;
        payloads[i].title = title;
        payloads[i].sourceUrl = sourceUrl;
      }

      uint16_t retryAfterSeconds = 0;
      ApiStatus status = api_.createHighlights(payloads, ctx->itemCount, &retryAfterSeconds);

      if (status == ApiStatus::RateLimited && rateLimitRetryAvailable && hooks.sleepMs != nullptr) {
        rateLimitRetryAvailable = false;
        uint32_t waitMs = retryAfterSeconds > 0 ? retryAfterSeconds * 1000u : DEFAULT_RATE_LIMIT_WAIT_MS;
        if (waitMs > RATE_LIMIT_WAIT_CAP_MS) {
          waitMs = RATE_LIMIT_WAIT_CAP_MS;
        }
        hooks.sleepMs(hooks.ctx, waitMs);
        status = api_.createHighlights(payloads, ctx->itemCount, &retryAfterSeconds);
      }

      if (status != ApiStatus::Ok) {
        // Abandon the pass; every un-acknowledged record stays pending and the
        // next sync retries from here.
        outcome.status = status;
        outcome.failed += static_cast<uint16_t>(ctx->itemCount);
        return outcome;
      }

      // A torn patch here re-uploads at worst one batch next sync (dedup'd
      // server-side), never loses a highlight.
      bool patchesOk = true;
      for (size_t i = 0; i < ctx->itemCount; ++i) {
        patchesOk = store_.markUploaded(id.data(), ctx->items[i].recFileOffset) && patchesOk;
      }
      outcome.uploaded += static_cast<uint16_t>(ctx->itemCount);
      if (!patchesOk) {
        // The next walk would re-collect the same records and POST forever.
        // Leave the document pending and move on; next sync re-uploads the
        // unmarked records, which the API dedups.
        outcome.failed++;
        break;
      }
    }
  }

  outcome.ok = outcome.failed == 0;
  return outcome;
}

}  // namespace readwise
