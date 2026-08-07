#include "ReadwiseSyncActivity.h"

#include <GfxRenderer.h>
#include <HttpReadwiseApi.h>
#include <I18n.h>
#include <Memory.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>
#include <WiFi.h>

#include <cstdio>

#include "ReadwiseCredentialStore.h"
#include "ReadwiseImageFetcher.h"
#include "ReadwiseSupport.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"

void ReadwiseSyncActivity::onEnter() {
  Activity::onEnter();

  if (!READWISE_STORE.hasToken()) {
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }

  wifiActivated = true;
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    onWifiSelectionComplete(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadwiseSyncActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    // Shed the WiFi/TLS heap fragmentation and land back in the Readwise
    // library rather than Home.
    silentRestartToReadwise();
  }
}

void ReadwiseSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    RenderLock lock(*this);
    state = State::FAILED;
    statusMessage = tr(STR_READWISE_NO_WIFI);
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    state = State::SYNCING;
  }
  // Paint the syncing screen before the blocking work starts, or the user
  // stares at the WiFi picker for the whole sync.
  requestUpdateAndWait();
  performSync();
  // The sync blocked the main loop for its whole duration; without this the
  // inactivity timer is already past the timeout and the result screen would
  // be replaced by the sleep screen immediately.
  activityManager.noteBlockingWorkFinished();
}

void ReadwiseSyncActivity::performSync() {
  // The engine is ~2 KB of scratch buffers -- heap, never stack, and released
  // before the result screen so the reader that follows gets the memory back.
  readwise::SdReadwiseFileStore store;
  readwise::HttpReadwiseApi api(READWISE_STORE.getToken());
  auto engine = makeUniqueNoThrow<readwise::ReadwiseSyncEngine>(api, store, ReadwiseCredentialStore::getDataDir());
  if (!engine) {
    LOG_ERR("RWSYNC", "OOM: sync engine");
    RenderLock lock(*this);
    state = State::FAILED;
    statusMessage = tr(STR_READWISE_SYNC_FAILED);
    requestUpdate();
    return;
  }
  engine->setDocumentCap(READWISE_STORE.getDocumentCap());

  const readwise::SyncOutcome outcome = engine->sync();

  if (outcome.ok) {
    // Sync is the only online operation: fetch every missing article body now
    // so the whole library reads offline afterwards. Throttling is expected --
    // body fetches share the list endpoint's 20 req/min budget -- and the
    // engine waits out retry-after between documents via the sleep hook.
    {
      RenderLock lock(*this);
      state = State::DOWNLOADING_BODIES;
      pushed = outcome.pushed;
      pulled = outcome.pulled;
    }
    requestUpdateAndWait();

    readwise::ReadwiseSyncEngine::BodySyncHooks hooks;
    hooks.ctx = this;
    hooks.onProgress = [](void* ctx, uint16_t done, uint16_t total) {
      auto* self = static_cast<ReadwiseSyncActivity*>(ctx);
      {
        RenderLock lock(*self);
        self->bodiesDone = done;
        self->bodiesTotal = total;
      }
      // Immediate: a deferred update only fires when loop() returns, and this
      // whole pass runs blocking inside one loop() iteration -- deferring left
      // the popup frozen at its initial 0/0 for the entire download.
      self->requestUpdate(true);
    };
    hooks.sleepMs = [](void*, uint32_t ms) { delay(ms); };
    // Wi-Fi is already up for the metadata pass, so article images ride along
    // with it and reading stays fully offline afterwards.
    ReadwiseUi::HttpArticleImageFetcher imageFetcher;
    hooks.imageFetcher = &imageFetcher;
    const auto bodies = engine->downloadMissingBodies(hooks);
    engine.reset();

    RenderLock lock(*this);
    bodiesDone = bodies.downloaded;
    bodiesTotal = bodies.total;
    bodiesFailed = bodies.failed;
    if (bodies.ok) {
      state = State::COMPLETE;
    } else {
      // Metadata committed; only the body pass failed. Report the cause; the
      // downloaded articles stay cached and the rest retry on demand.
      state = State::FAILED;
      statusMessage = I18N.get(ReadwiseUi::statusStrId(bodies.status));
    }
    requestUpdate();
    return;
  }
  engine.reset();

  RenderLock lock(*this);
  {
    state = State::FAILED;
    statusMessage = outcome.status == readwise::ApiStatus::Ok ? tr(STR_READWISE_SYNC_FAILED)
                                                              : I18N.get(ReadwiseUi::statusStrId(outcome.status));
    LOG_ERR("RWSYNC", "Sync failed: stage=%u status=%s", static_cast<unsigned>(outcome.failedStage),
            readwise::apiStatusName(outcome.status));
  }
  requestUpdate();
}

void ReadwiseSyncActivity::loop() {
  if (state == State::COMPLETE || state == State::FAILED || state == State::NO_TOKEN) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Pop back to the library; onExit's silent restart lands there with a
      // defragmented heap when WiFi was brought up.
      finish();
    }
  }
}

void ReadwiseSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READWISE_SYNC));

  switch (state) {
    case State::CONNECTING:
      GUI.drawPopup(renderer, tr(STR_CONNECTING));
      break;
    case State::SYNCING:
      GUI.drawPopup(renderer, tr(STR_READWISE_SYNCING));
      break;
    case State::DOWNLOADING_BODIES: {
      // drawPopup does not wrap, so keep this short: "Downloading article... 3/12".
      char progress[64];
      snprintf(progress, sizeof(progress), "%s %u/%u", tr(STR_READWISE_DOWNLOADING), (unsigned)bodiesDone,
               (unsigned)bodiesTotal);
      GUI.drawPopup(renderer, progress);
      break;
    }
    case State::COMPLETE: {
      char summary[64];
      if (bodiesFailed > 0) {
        snprintf(summary, sizeof(summary), "%s (%u/%u, -%u)", tr(STR_READWISE_SYNC_COMPLETE), (unsigned)bodiesDone,
                 (unsigned)bodiesTotal, (unsigned)bodiesFailed);
      } else {
        snprintf(summary, sizeof(summary), "%s (%u/%u)", tr(STR_READWISE_SYNC_COMPLETE), (unsigned)bodiesDone,
                 (unsigned)bodiesTotal);
      }
      GUI.drawPopup(renderer, summary);
      break;
    }
    case State::FAILED:
      GUI.drawPopup(renderer, statusMessage.c_str());
      break;
    case State::NO_TOKEN:
      GUI.drawPopup(renderer, tr(STR_READWISE_SET_TOKEN_FIRST));
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
