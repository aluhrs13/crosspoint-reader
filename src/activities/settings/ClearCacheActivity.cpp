#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <NullReadwiseApi.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>

#include "MappedInputManager.h"
#include "ReadwiseCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

StrId ClearCacheActivity::titleStr() const {
  return mode == Mode::ReadwiseArticles ? StrId::STR_CLEAR_READWISE_ARTICLES : StrId::STR_CLEAR_READING_CACHE;
}

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  const char* options[] = {tr(STR_CANCEL), tr(STR_CLEAR_BUTTON)};
  confirmPopup.show(I18N.get(titleStr()), options, 2, 0, [this](int idx) {
    if (idx == 1) {
      beginClear();
    } else {
      goBack();
    }
  });
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, I18N.get(titleStr()));

  if (state == WARNING) {
    const bool articles = mode == Mode::ReadwiseArticles;
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60,
                              articles ? tr(STR_CLEAR_ARTICLES_WARNING_1) : tr(STR_CLEAR_CACHE_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30,
                              articles ? tr(STR_CLEAR_ARTICLES_WARNING_2) : tr(STR_CLEAR_CACHE_WARNING_2), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10,
                              articles ? tr(STR_CLEAR_ARTICLES_WARNING_3) : tr(STR_CLEAR_CACHE_WARNING_3), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30,
                              articles ? tr(STR_CLEAR_ARTICLES_WARNING_4) : tr(STR_CLEAR_CACHE_WARNING_4), true);

    if (confirmPopup.processRender(renderer, mappedInput)) return;

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_CLEARED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEAR_CACHE_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::beginClear() {
  LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
  {
    RenderLock lock(*this);
    state = CLEARING;
  }
  requestUpdateAndWait();
  clearCache();
}

void ClearCacheActivity::clearCache() {
  clearedCount = 0;
  failedCount = 0;
  if (mode == Mode::ReadwiseArticles) {
    clearReadwiseArticles();
  } else {
    clearBookCaches();
  }
}

// Deletes each downloaded article directory (archive, section cache, extracted
// images) and clears its cached-body flag, so the library stops showing the
// article as downloaded and re-fetches it on next open.
void ClearCacheActivity::clearReadwiseArticles() {
  const std::string bodiesDir = std::string(ReadwiseCredentialStore::getDataDir()) + "/bodies";
  LOG_DBG("CLEAR_CACHE", "Clearing Readwise articles in %s", bodiesDir.c_str());

  auto root = Storage.open(bodiesDir.c_str());
  if (!root || !root.isDirectory()) {
    // No articles downloaded yet is a success with nothing to do, not a failure.
    LOG_DBG("CLEAR_CACHE", "No Readwise bodies directory");
    if (root) root.close();
    state = SUCCESS;
    requestUpdate();
    return;
  }

  readwise::NullReadwiseApi nullApi;
  readwise::SdReadwiseFileStore store;
  auto engine = makeUniqueNoThrow<readwise::ReadwiseSyncEngine>(nullApi, store, ReadwiseCredentialStore::getDataDir());
  if (!engine) {
    // Without the engine the flags cannot be cleared, and deleting the files
    // anyway would leave the library claiming the articles are downloaded.
    LOG_ERR("CLEAR_CACHE", "OOM: sync engine");
    root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  char name[128];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (!entry.isDirectory()) {
      entry.close();
      continue;
    }
    // The directory name is the document id.
    const std::string id(name);
    const std::string fullPath = bodiesDir + "/" + id;
    entry.close();  // must close before removing the same path

    if (Storage.removeDir(fullPath.c_str())) {
      engine->setBodyCached(id.c_str(), false);
      clearedCount++;
    } else {
      LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
      failedCount++;
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "Articles cleared: %d removed, %d failed", clearedCount, failedCount);
  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::clearBookCaches() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  // Open .crosspoint directory
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories matching known book cache names.
    if (file.isDirectory() && isBookCacheDirectoryName(itemName.c_str())) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (Storage.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      beginClear();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      goBack();
    }
    return;
  }
}
