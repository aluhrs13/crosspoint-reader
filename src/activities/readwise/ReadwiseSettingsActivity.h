#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Device-side Readwise configuration: token entry (on-device keyboard; the
 * local web settings page is the comfortable path for a 50-character token),
 * connection test, Home-menu visibility, and the document cap.
 */
class ReadwiseSettingsActivity final : public Activity {
 public:
  explicit ReadwiseSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadwiseSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  void testConnection();

  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  enum class AuthState : uint8_t { UNTESTED, TESTING, OK, FAILED };
  AuthState authState = AuthState::UNTESTED;
  std::string authDetail;
  bool wifiActivated = false;
};
