#pragma once

#include <functional>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ClearCacheActivity final : public Activity {
 public:
  // BookCaches drops the derived render caches beside each book, keeping the
  // books themselves. ReadwiseArticles deletes the downloaded articles as well,
  // because for a Readwise article the downloaded EPUB *is* the content and
  // there is nothing else to regenerate it from.
  enum class Mode : uint8_t { BookCaches, ReadwiseArticles };

  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode = Mode::BookCaches)
      : Activity("ClearCache", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  State state = WARNING;
  Mode mode = Mode::BookCaches;

  void goBack() { finish(); }

  // The screen title, reused for the confirm prompt and the header.
  StrId titleStr() const;

  int clearedCount = 0;
  int failedCount = 0;
  OptionPopup confirmPopup;
  void beginClear();
  void clearCache();
  void clearBookCaches();
  void clearReadwiseArticles();
};
