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
  engine.reset();

  RenderLock lock(*this);
  if (outcome.ok) {
    state = State::COMPLETE;
    pushed = outcome.pushed;
    pulled = outcome.pulled;
  } else {
    state = State::FAILED;
    switch (outcome.status) {
      case readwise::ApiStatus::AuthFailed:
        statusMessage = tr(STR_READWISE_AUTH_FAILED);
        break;
      case readwise::ApiStatus::RateLimited:
        statusMessage = tr(STR_READWISE_RATE_LIMITED);
        break;
      case readwise::ApiStatus::LowMemory:
        statusMessage = tr(STR_READWISE_LOW_MEMORY);
        break;
      default:
        statusMessage = tr(STR_READWISE_SYNC_FAILED);
        break;
    }
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
    case State::COMPLETE: {
      char summary[96];
      snprintf(summary, sizeof(summary), "%s (%u/%u)", tr(STR_READWISE_SYNC_COMPLETE), (unsigned)pushed,
               (unsigned)pulled);
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
