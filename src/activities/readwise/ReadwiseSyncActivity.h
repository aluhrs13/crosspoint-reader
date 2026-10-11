#pragma once

#include <ReadwiseDocument.h>

namespace readwise {
struct PendingOp;
}

#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

/**
 * Explicit Readwise synchronization: connect Wi-Fi if needed, run the sync
 * engine (push queued ops, pull changed metadata, rebuild indexes, commit the
 * checkpoint), show the outcome.
 *
 * Follows KOReaderSyncActivity's shape: blocking network work on the main loop
 * task with RenderLock'd state updates, and the WiFi teardown + silent restart
 * in onExit() to shed heap fragmentation. A failed sync preserves the previous
 * checkpoint and the pending queue, except a document the server no longer has:
 * that one prompts Stop or Continue, and Continue drops it so the next sync
 * does not try it again.
 */
class ReadwiseSyncActivity final : public Activity {
 public:
  explicit ReadwiseSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadwiseSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == State::CONNECTING || state == State::SYNCING || state == State::SKIP_PROMPT ||
           state == State::DOWNLOADING_BODIES;
  }

 private:
  enum class State : uint8_t {
    CONNECTING,
    SYNCING,
    SKIP_PROMPT,
    DOWNLOADING_BODIES,
    COMPLETE,
    FAILED,
    NO_TOKEN,
  };

  void onWifiSelectionComplete(bool connected);
  void performSync();
  // True when the user wants this 404 dropped and the sync to continue.
  bool promptDropMissing(const char* id, const char* title, const char* detail);
  static bool onNotFound(void* ctx, const readwise::PendingOp& op, const char* title, const char* detail);
  void renderComplete() const;
  void renderDownloading() const;
  void renderFailed() const;

  enum class DownloadStep : uint8_t { Article, Image, RateLimit };

  State state = State::CONNECTING;
  std::string statusMessage;
  // Step and server snippet when sync fails. Diagnostic text, not a translation.
  // Same width as SyncOutcome::detail; the cpp static_asserts that.
  static constexpr size_t FAILURE_DETAIL_CAP = 160;
  char failureDetail[FAILURE_DETAIL_CAP] = {};
  // Dialog body for a 404. The activity is heap-allocated; the title plus the
  // explanation do not belong on the stack.
  static constexpr size_t SKIP_MESSAGE_CAP = 320;
  char skipMessage[SKIP_MESSAGE_CAP] = {};
  OptionPopup skipPopup;
  uint16_t pushed = 0;
  uint16_t pulled = 0;
  uint16_t highlightsSent = 0;
  uint16_t highlightsFailed = 0;
  char highlightDetail[FAILURE_DETAIL_CAP] = {};
  uint16_t bodiesDone = 0;
  uint16_t bodiesTotal = 0;
  uint16_t bodiesFailed = 0;
  // What the body pass is doing right now. Copied out of the engine callback
  // because the title pointer there dies when the callback returns.
  DownloadStep downloadStep = DownloadStep::Article;
  uint16_t stepIndex = 0;
  uint16_t stepCount = 0;
  uint32_t currentWords = 0;
  readwise::Category currentCategory = readwise::Category::Unknown;
  char currentTitle[readwise::TITLE_CAP] = {};
  // Title of the first article that failed, named on the summary screen.
  std::string failedTitle;
  bool wifiActivated = false;
};
