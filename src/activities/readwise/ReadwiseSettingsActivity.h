#pragma once

#include <string>

#include "activities/UiListActivity.h"

/**
 * Device-side Readwise configuration: token entry (on-device keyboard; the
 * local web settings page is the comfortable path for a 50-character token),
 * connection test, Home-menu visibility, and the document cap.
 */
class ReadwiseSettingsActivity final : public UiListActivity {
 public:
  explicit ReadwiseSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("ReadwiseSettings", renderer, mappedInput) {}

  static constexpr int MENU_COUNT = 4;

  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return MENU_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void testConnection();
  const char* rowValue(int index);

  freeink::ui::ListItem rowItems_[MENU_COUNT]{};
  // Composed values that are not a stable tr() string: the bracketed
  // "set token first" hint and the decimal document cap.
  char statusBuf[96]{};
  char capBuf[8]{};

  enum class AuthState : uint8_t { UNTESTED, TESTING, OK, FAILED };
  AuthState authState = AuthState::UNTESTED;
  std::string authDetail;
  bool wifiActivated = false;
};
