#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * Explicit Readwise synchronization: connect Wi-Fi if needed, run the sync
 * engine (push queued ops, pull changed metadata, rebuild indexes, commit the
 * checkpoint), show the outcome.
 *
 * Follows KOReaderSyncActivity's shape: blocking network work on the main loop
 * task with RenderLock'd state updates, no cancellation once started, and the
 * WiFi teardown + silent restart in onExit() to shed heap fragmentation. A
 * failed sync preserves the previous checkpoint and the pending queue by
 * construction (that is the engine's contract, proven by the native tests).
 */
class ReadwiseSyncActivity final : public Activity {
 public:
  explicit ReadwiseSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadwiseSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::CONNECTING || state == State::SYNCING; }

 private:
  enum class State : uint8_t {
    CONNECTING,
    SYNCING,
    COMPLETE,
    FAILED,
    NO_TOKEN,
  };

  void onWifiSelectionComplete(bool connected);
  void performSync();

  State state = State::CONNECTING;
  std::string statusMessage;
  uint16_t pushed = 0;
  uint16_t pulled = 0;
  bool wifiActivated = false;
};
