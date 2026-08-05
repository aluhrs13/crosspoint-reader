#include "ReadwiseSettingsActivity.h"

#include <GfxRenderer.h>
#include <HttpReadwiseApi.h>
#include <I18n.h>
#include <WiFi.h>

#include "ReadwiseCredentialStore.h"
#include "ReadwiseSupport.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace {
constexpr int MENU_ITEMS = 4;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_READWISE_TOKEN, StrId::STR_READWISE_TEST_CONNECTION,
                                     StrId::STR_READWISE_SHOW_IN_HOME, StrId::STR_READWISE_DOCUMENT_CAP};

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

void ReadwiseSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  authState = AuthState::UNTESTED;
  requestUpdate();
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

void ReadwiseSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int touchSel = static_cast<int>(selectedIndex);
  const auto listTouch = handleListTouch(touchSel, MENU_ITEMS, contentTop, contentHeight, false);
  if (listTouch != ListTouchResult::None) {
    selectedIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) {
      handleSelection();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void ReadwiseSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
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
  } else if (selectedIndex == 1) {
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
  } else if (selectedIndex == 2) {
    READWISE_STORE.setSyncEnabled(!READWISE_STORE.isSyncEnabled());
    READWISE_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 3) {
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

void ReadwiseSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READWISE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, static_cast<int>(selectedIndex),
      [](const int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [this](const int index) -> std::string {
        if (index == 0) {
          return READWISE_STORE.hasToken() ? std::string("******") : std::string(tr(STR_NOT_SET));
        }
        if (index == 1) {
          if (!READWISE_STORE.hasToken()) {
            return std::string("[") + tr(STR_READWISE_SET_TOKEN_FIRST) + "]";
          }
          switch (authState) {
            case AuthState::UNTESTED:
              return "";
            case AuthState::TESTING:
              return tr(STR_READWISE_TESTING);
            case AuthState::OK:
              return tr(STR_READWISE_AUTH_OK);
            case AuthState::FAILED:
              return authDetail;
          }
          return "";
        }
        if (index == 2) {
          return READWISE_STORE.isSyncEnabled() ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
        }
        return std::to_string(READWISE_STORE.getDocumentCap());
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
