#include "ReadwiseSettingsActivity.h"

#include <GfxRenderer.h>
#include <HttpReadwiseApi.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdio>

#include "ReadwiseCredentialStore.h"
#include "ReadwiseSupport.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
const StrId menuNames[ReadwiseSettingsActivity::MENU_COUNT] = {
    StrId::STR_READWISE_TOKEN, StrId::STR_READWISE_TEST_CONNECTION, StrId::STR_READWISE_SHOW_IN_HOME,
    StrId::STR_READWISE_DOCUMENT_CAP};

// The cap cycles through fixed steps; free-form numeric entry is not worth a
// keyboard round trip.
constexpr uint16_t CAP_STEPS[] = {25, 50, 100, 200};

uint16_t nextCap(const uint16_t current) {
  for (size_t i = 0; i < sizeof(CAP_STEPS) / sizeof(CAP_STEPS[0]); ++i) {
    if (CAP_STEPS[i] == current) {
      return CAP_STEPS[(i + 1) % (sizeof(CAP_STEPS) / sizeof(CAP_STEPS[0]))];
    }
  }
  return CAP_STEPS[2];  // default 100
}
}  // namespace

const char* ReadwiseSettingsActivity::headerTitle() const { return tr(STR_READWISE); }

void ReadwiseSettingsActivity::onEnter() {
  authState = AuthState::UNTESTED;
  UiListActivity::onEnter();
}

void ReadwiseSettingsActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    // Same heap-defrag reboot as KOReaderAuthActivity after its network use.
    silentRestart();
  }
}

const char* ReadwiseSettingsActivity::rowValue(const int index) {
  if (index == 0) {
    return READWISE_STORE.hasToken() ? "******" : tr(STR_NOT_SET);
  }
  if (index == 1) {
    if (!READWISE_STORE.hasToken()) {
      snprintf(statusBuf, sizeof(statusBuf), "[%s]", tr(STR_READWISE_SET_TOKEN_FIRST));
      return statusBuf;
    }
    switch (authState) {
      case AuthState::UNTESTED:
        return nullptr;
      case AuthState::TESTING:
        return tr(STR_READWISE_TESTING);
      case AuthState::OK:
        return tr(STR_READWISE_AUTH_OK);
      case AuthState::FAILED:
        return authDetail.c_str();
    }
    return nullptr;
  }
  if (index == 2) {
    return READWISE_STORE.isSyncEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }
  snprintf(capBuf, sizeof(capBuf), "%u", static_cast<unsigned>(READWISE_STORE.getDocumentCap()));
  return capBuf;
}

void ReadwiseSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < MENU_COUNT; i++) {
    rowItems_[i] = {};
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].value = rowValue(i);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}

void ReadwiseSettingsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected.store(index);
  if (index == 0) {
    // Token entry. The web settings page is the comfortable path for a
    // 50-character opaque token; this keyboard path keeps the device
    // self-sufficient.
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READWISE_TOKEN),
                                                                   READWISE_STORE.getToken(), 64, InputType::Password),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               READWISE_STORE.setToken(kb.text);
                               READWISE_STORE.saveToFile();
                               authState = AuthState::UNTESTED;
                             }
                           });
  } else if (index == 1) {
    if (!READWISE_STORE.hasToken()) {
      return;
    }
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      testConnection();
      return;
    }
    wifiActivated = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               testConnection();
                             }
                           });
  } else if (index == 2) {
    READWISE_STORE.setSyncEnabled(!READWISE_STORE.isSyncEnabled());
    READWISE_STORE.saveToFile();
    requestUpdate();
  } else if (index == 3) {
    READWISE_STORE.setDocumentCap(nextCap(READWISE_STORE.getDocumentCap()));
    READWISE_STORE.saveToFile();
    requestUpdate();
  }
}

void ReadwiseSettingsActivity::testConnection() {
  {
    RenderLock lock(*this);
    authState = AuthState::TESTING;
  }
  requestUpdateAndWait();
  wifiActivated = true;

  readwise::HttpReadwiseApi api(READWISE_STORE.getToken());
  const readwise::ApiStatus status = api.checkAuth();

  RenderLock lock(*this);
  if (status == readwise::ApiStatus::Ok) {
    authState = AuthState::OK;
  } else {
    authState = AuthState::FAILED;
    authDetail = I18N.get(ReadwiseUi::statusStrId(status));
    LOG_ERR("RWSET", "Auth test failed: %s", readwise::apiStatusName(status));
  }
  requestUpdate();
}
