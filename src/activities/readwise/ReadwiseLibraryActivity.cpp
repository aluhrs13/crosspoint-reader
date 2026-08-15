#include "ReadwiseLibraryActivity.h"

#include <ArticleAssembler.h>
#include <ArticleBodyWriter.h>
#include <GfxRenderer.h>
#include <HttpReadwiseApi.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "CrossPointState.h"
#include "ReadwiseCredentialStore.h"
#include "ReadwiseImageFetcher.h"
#include "ReadwiseSupport.h"
#include "ReadwiseSyncActivity.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Confirm held this long queues an archive instead of opening. Matches the
// reader's GO_HOME_MS long-press feel.
// Shared by every press-and-hold action on this screen: archive (Confirm) and
// send-to-location (Left/Right).
constexpr unsigned long HOLD_ACTION_MS = 1000;
// One window of metadata; sized generously past a visible page.
constexpr int WINDOW_SIZE = 32;

const char* locationLabel(const readwise::Location location) {
  switch (location) {
    case readwise::Location::Shortlist:
      return tr(STR_READWISE_SHORTLIST);
    case readwise::Location::Feed:
      return tr(STR_READWISE_FEED);
    default:
      return tr(STR_READWISE_LATER);
  }
}
}  // namespace

// Bodies used to be plain text at bodies/<id>.txt; they are now EPUB archives
// at bodies/<id>/article.epub. The old files no longer match any path the
// library looks for, so without this they would sit on the card forever with
// nothing owning them. Clearing the flag makes the next sync re-fetch the
// article -- this time with its images.
void ReadwiseLibraryActivity::migrateLegacyTextBodies() {
  const std::string bodiesDir = std::string(ReadwiseCredentialStore::getDataDir()) + "/bodies";
  HalFile dir = Storage.open(bodiesDir.c_str());
  if (!dir || !dir.isDirectory()) {
    return;
  }

  int migrated = 0;
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    char name[64];
    if (entry.isDirectory() || entry.getName(name, sizeof(name)) == 0) {
      continue;
    }
    const std::string fileName(name);
    if (fileName.size() < 5 || fileName.compare(fileName.size() - 4, 4, ".txt") != 0) {
      continue;
    }
    const std::string id = fileName.substr(0, fileName.size() - 4);
    entry.close();  // must close before removing the same path
    if (Storage.remove((bodiesDir + "/" + fileName).c_str())) {
      ++migrated;
    }
    if (engine) {
      engine->setBodyCached(id.c_str(), false);
    }
  }
  if (migrated > 0) {
    LOG_INF("RWLIB", "Migrated %d legacy text bodies; they re-download with images", migrated);
  }
}

void ReadwiseLibraryActivity::onEnter() {
  Activity::onEnter();
  engine = makeUniqueNoThrow<readwise::ReadwiseSyncEngine>(nullApi, store, ReadwiseCredentialStore::getDataDir());
  if (!engine) {
    LOG_ERR("RWLIB", "OOM: sync engine; library will show empty");
  }
  if (engine) {
    engine->setDocumentCap(READWISE_STORE.getDocumentCap());
    // Reflect any actions queued in a previous session that a failed sync
    // left visible-state stale.
    engine->rebuildLocal();
  }
  migrateLegacyTextBodies();
  // Return to the view the last article was opened from rather than always
  // Later -- including after the restart that an uncached open performs.
  constexpr auto locationCount = static_cast<uint8_t>(std::size(LOCATIONS));
  locationIndex =
      APP_STATE.readwiseLocationIndex < locationCount ? static_cast<int>(APP_STATE.readwiseLocationIndex) : 0;
  selectedIndex = 0;
  state = State::LIST;
  // Entered by a Back press that is very likely still held; see the member.
  ignoreBackUntilReleased = mappedInput.isPressed(MappedInputManager::Button::Back);
  reloadCounts();
  requestUpdate();
}

void ReadwiseLibraryActivity::onExit() {
  Activity::onExit();
  rememberLocation();
  window.clear();
  engine.reset();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    // Shed the WiFi/TLS heap fragmentation; boots straight back into this
    // library.
    silentRestartToReadwise();
  }
}

void ReadwiseLibraryActivity::rememberLocation() {
  // Value-change guarded: switching views with Left/Right must not rewrite
  // state.json on every press.
  const auto current = static_cast<uint8_t>(locationIndex);
  if (APP_STATE.readwiseLocationIndex == current) {
    return;
  }
  APP_STATE.readwiseLocationIndex = current;
  APP_STATE.saveToFile();
}

void ReadwiseLibraryActivity::reloadCounts() {
  docCount = engine ? engine->indexCount(LOCATIONS[locationIndex]) : 0;
  window.clear();
  windowStart = 0;
  if (selectedIndex >= totalRows()) {
    selectedIndex = totalRows() - 1;
  }
  if (selectedIndex < 0) {
    selectedIndex = 0;
  }
}

void ReadwiseLibraryActivity::ensureWindow(const int docIndex) {
  if (engine == nullptr || docIndex < 0 || docIndex >= docCount) {
    return;
  }
  if (docIndex >= windowStart && docIndex < windowStart + static_cast<int>(window.size())) {
    return;
  }
  windowStart = (docIndex / WINDOW_SIZE) * WINDOW_SIZE;
  const uint16_t wanted = static_cast<uint16_t>(std::min<int>(WINDOW_SIZE, static_cast<int>(docCount) - windowStart));
  if (!engine->readIndexPage(LOCATIONS[locationIndex], static_cast<uint16_t>(windowStart), wanted, window)) {
    window.clear();
  }
}

const readwise::Document* ReadwiseLibraryActivity::docAt(const int docIndex) {
  ensureWindow(docIndex);
  const int rel = docIndex - windowStart;
  if (rel < 0 || rel >= static_cast<int>(window.size())) {
    return nullptr;
  }
  return &window[static_cast<size_t>(rel)];
}

void ReadwiseLibraryActivity::jumpToLocation(const int index) {
  locationIndex = index;
  selectedIndex = 0;
  reloadCounts();
  requestUpdate();
}

void ReadwiseLibraryActivity::loop() {
  if (ignoreBackUntilReleased) {
    // Skip this frame entirely so the in-flight Back release is consumed
    // without acting on it.
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) {
      ignoreBackUntilReleased = false;
    }
    return;
  }

  if (state == State::DOWNLOAD_FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = State::LIST;
      requestUpdate();
    }
    return;
  }
  if (state == State::DOWNLOADING) {
    // performDownload() runs synchronously from activateSelection(); nothing
    // to poll here.
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
#ifdef CROSSPOINT_READWISE_ONLY
    // This library IS home in the Readwise-only build; Back opens Settings
    // (whose Back returns here via the re-routed goHome()).
    activityManager.goToSettings();
#else
    onGoHome();
#endif
    return;
  }

  // Long-press Confirm archives, fired WHILE held -- the convention every
  // other activity uses (see EpubReaderActivity's long-press menu function).
  // Checking held time on release instead looked equivalent but never
  // triggered in the hand.
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    if (!holdActionTriggered && selectedIndex > 0 && mappedInput.getHeldTime() >= HOLD_ACTION_MS) {
      const readwise::Document* doc = docAt(selectedIndex - 1);
      if (doc != nullptr) {
        holdActionTriggered = true;  // suppress the release below
        queueMove(*doc, readwise::Location::Archive);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (holdActionTriggered) {
      holdActionTriggered = false;  // the hold already acted
      return;
    }
    activateSelection();
    return;
  }

  if (handleLocationHold(MappedInputManager::Button::Left, leftTargetIndex())) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (holdActionTriggered) {
      holdActionTriggered = false;  // the hold already moved the article
      return;
    }
    jumpToLocation(leftTargetIndex());
    return;
  }
  if (handleLocationHold(MappedInputManager::Button::Right, rightTargetIndex())) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (holdActionTriggered) {
      holdActionTriggered = false;
      return;
    }
    jumpToLocation(rightTargetIndex());
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int touchSel = selectedIndex;
  const auto listTouch = handleListTouch(touchSel, totalRows(), contentTop, contentHeight, true);
  if (listTouch != ListTouchResult::None) {
    selectedIndex = touchSel;
    if (listTouch == ListTouchResult::Activated) {
      activateSelection();
    }
    return;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalRows());
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalRows());
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalRows(), pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalRows(), pageItems);
    requestUpdate();
  });
}

void ReadwiseLibraryActivity::activateSelection() {
  if (selectedIndex == 0) {
    activityManager.pushActivity(std::make_unique<ReadwiseSyncActivity>(renderer, mappedInput));
    return;
  }
  const readwise::Document* doc = docAt(selectedIndex - 1);
  if (doc == nullptr) {
    return;
  }
  openDocument(*doc);
}

void ReadwiseLibraryActivity::openDocument(const readwise::Document& doc) {
  if (engine == nullptr) {
    return;
  }
  const std::string bodyPath = ReadwiseUi::bodyPathForId(doc.id);
  if (!bodyPath.empty() && (doc.flags & readwise::FLAG_HAS_BODY) != 0 && store.exists(bodyPath)) {
    // Cached: open offline. `seen` queues only on an open that actually
    // happens -- never on a cancelled Wi-Fi picker or a failed download --
    // and only when the server does not already report the document opened
    // (first_opened_at parses into FLAG_SEEN).
    if ((doc.flags & readwise::FLAG_SEEN) == 0) {
      engine->queueSeen(doc.id, doc.updatedAt);
    }
    // ReaderActivity recognizes the managed path and routes Back here.
    activityManager.goToReader(bodyPath);
    return;
  }
  startDownload(doc);
}

void ReadwiseLibraryActivity::startDownload(const readwise::Document& doc) {
  pendingDownloadId = doc.id;
  pendingDownloadTitle = doc.title;
  pendingDownloadRev = doc.updatedAt;
  pendingDownloadSourceUrl = doc.sourceUrl;
  pendingDownloadAuthor = doc.author;
  pendingDownloadSeen = (doc.flags & readwise::FLAG_SEEN) != 0;
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    performDownload();
    return;
  }
  wifiActivated = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             pendingDownloadId.clear();
                             return;
                           }
                           performDownload();
                         });
}

// Images can take several seconds each on a slow connection; without this the
// popup would sit on the title alone and read as a hang.
void ReadwiseLibraryActivity::sImageProgress(void* ctx, size_t done, size_t total) {
  auto* self = static_cast<ReadwiseLibraryActivity*>(ctx);
  if (total == 0) {
    return;
  }
  {
    RenderLock lock(*self);
    self->statusMessage = self->pendingDownloadTitle + " (" + std::to_string(done) + "/" + std::to_string(total) + ")";
  }
  self->requestUpdate();
}

void ReadwiseLibraryActivity::performDownload() {
  {
    RenderLock lock(*this);
    state = State::DOWNLOADING;
    statusMessage = pendingDownloadTitle;
  }
  requestUpdateAndWait();
  wifiActivated = true;

  const std::string bodyPath = ReadwiseUi::bodyPathForId(pendingDownloadId.c_str());
  const std::string articleDir = ReadwiseUi::articleDirForId(pendingDownloadId.c_str());
  readwise::ApiStatus status = readwise::ApiStatus::LowMemory;
  if (!bodyPath.empty()) {
    // Nothing else creates these directories -- sync only ensures the base
    // dir -- and SdFat's open-for-write fails outright on a missing parent,
    // which aborted the very first article download as a ParseError.
    store.ensureDir(std::string(ReadwiseCredentialStore::getDataDir()) + "/bodies");
    store.ensureDir(articleDir);
    const std::string xhtmlPath = articleDir + "/.body.xhtml";
    const std::string scratchPath = articleDir + "/.img.tmp";

    readwise::HttpReadwiseApi api(READWISE_STORE.getToken());
    // The writer carries the tokenizer, the XHTML emitter, and the image URL
    // arena (~4.6 KB) and sits under a live TLS session -- heap, not the
    // main-loop stack.
    auto writer = makeUniqueNoThrow<readwise::ArticleBodyWriter>(store, xhtmlPath, pendingDownloadSourceUrl.c_str(),
                                                                 pendingDownloadTitle.c_str());
    if (!writer) {
      LOG_ERR("RWLIB", "OOM: body writer");
    } else {
      uint16_t retryAfter = 0;
      status = api.fetchBody(pendingDownloadId.c_str(), *writer, &retryAfter);
      if (status == readwise::ApiStatus::Ok && !writer->committed()) {
        // A 200 whose body never arrived (document without html_content).
        status = readwise::ApiStatus::ParseError;
      }
      if (status == readwise::ApiStatus::Ok) {
        // Images come after the body's TLS session closes: a nested request
        // inside the read callback is impossible, and this is the heap's worst
        // moment. Individual image failures degrade to alt text and must not
        // cost the article, so only assembly itself can fail the download.
        ReadwiseUi::HttpArticleImageFetcher fetcher;
        const readwise::ArticleAssemblyResult assembly = readwise::assembleArticleEpub(
            store, fetcher, *writer, xhtmlPath, scratchPath, bodyPath, pendingDownloadTitle.c_str(),
            pendingDownloadAuthor.c_str(), &sImageProgress, this);
        if (!assembly.ok) {
          store.remove(xhtmlPath);
          store.remove(scratchPath);
          status = readwise::ApiStatus::ParseError;
        } else {
          LOG_INF("RWLIB", "Article assembled: %u/%u images", static_cast<unsigned>(assembly.imagesStored),
                  static_cast<unsigned>(assembly.imagesRequested));
        }
      }
    }
  }

  // The fetch blocked the main loop for its whole duration; without this the
  // inactivity timer is already past the timeout and the device would sleep
  // the instant control returns, hiding the outcome.
  activityManager.noteBlockingWorkFinished();

  if (status == readwise::ApiStatus::Ok) {
    // Persist the body flag in docs.bin, or the restart below would show the
    // article as not downloaded and fetch it again on reopen.
    if (engine) {
      engine->setBodyCached(pendingDownloadId.c_str(), true);
      // The download succeeded, so this open is real: queue `seen` now unless
      // the server already reported the document opened.
      if (!pendingDownloadSeen) {
        engine->queueSeen(pendingDownloadId.c_str(), pendingDownloadRev.c_str());
      }
    }
    APP_STATE.openEpubPath = bodyPath;
    // The restart below skips onExit(), so record the view here or Back out of
    // the article would land on Later instead of the one it was opened from.
    APP_STATE.readwiseLocationIndex = static_cast<uint8_t>(locationIndex);
    APP_STATE.saveToFile();
    // The WiFi/TLS session just fragmented the heap the reader needs; the
    // silent restart both sheds it and lands directly in the managed reader.
    WiFi.disconnect(false);
    delay(30);
    wifiActivated = false;
    silentRestartToReader();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::DOWNLOAD_FAILED;
    statusMessage = I18N.get(ReadwiseUi::statusStrId(status));
  }
  LOG_ERR("RWLIB", "Download failed: %s", readwise::apiStatusName(status));
  requestUpdate();
}

bool ReadwiseLibraryActivity::handleLocationHold(const MappedInputManager::Button button, const int targetIndex) {
  if (!mappedInput.isPressed(button)) {
    return false;
  }
  const readwise::Location target = LOCATIONS[targetIndex];
  // Feed is server-side content rather than a shelf, so it is a view you can
  // switch to but never a destination you can send an article to.
  const bool movable = target == readwise::Location::Later || target == readwise::Location::Shortlist;
  if (movable && !holdActionTriggered && selectedIndex > 0 && mappedInput.getHeldTime() >= HOLD_ACTION_MS) {
    const readwise::Document* doc = docAt(selectedIndex - 1);
    if (doc != nullptr) {
      holdActionTriggered = true;  // suppress the release that follows
      queueMove(*doc, target);
    }
  }
  return true;  // held: swallow the frame either way
}

void ReadwiseLibraryActivity::queueMove(const readwise::Document& doc, const readwise::Location target) {
  if (engine == nullptr) {
    return;
  }
  if (!engine->queueLocationChange(doc.id, target, doc.updatedAt)) {
    return;
  }
  // Visible immediately: the document leaves the current index now, and the
  // queued op pushes at the next sync.
  engine->rebuildLocal();
  reloadCounts();
  requestUpdate();
}

void ReadwiseLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect headerRect{0, metrics.topPadding, pageWidth, metrics.headerHeight};
  // The Left/Right hints name the destination view, so the button for the
  // current view reads as the way back to Later.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), locationLabel(LOCATIONS[leftTargetIndex()]),
                                            locationLabel(LOCATIONS[rightTargetIndex()]));

  if (state == State::DOWNLOADING) {
    GUI.drawHeader(renderer, headerRect, tr(STR_READWISE_LIBRARY));
    GUI.drawPopup(renderer, tr(STR_READWISE_DOWNLOADING));
    renderer.displayBuffer();
    return;
  }
  if (state == State::DOWNLOAD_FAILED) {
    GUI.drawHeader(renderer, headerRect, tr(STR_READWISE_LIBRARY));
    // drawPopup sizes to its text with no wrapping: the combined
    // "Download failed: <reason>" overflowed the 480px portrait width and
    // spammed per-pixel GFX clip errors. The translated reason alone fits and
    // says enough.
    GUI.drawPopup(renderer, statusMessage.c_str());
    renderer.displayBuffer();
    return;
  }

  const std::string header = std::string(tr(STR_READWISE_LIBRARY)) + " - " + locationLabel(LOCATIONS[locationIndex]);
  GUI.drawHeader(renderer, headerRect, header.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  // Preload the window for the visible page from the render-side index math so
  // the row lambdas below never touch the SD card mid-draw.
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
  const int pageStart = (selectedIndex / pageItems) * pageItems;
  ensureWindow(pageStart - 1);
  ensureWindow(std::min(pageStart + pageItems - 1, totalRows() - 1) - 1);

  if (docCount == 0) {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, 1, selectedIndex,
                 [](int) { return std::string(tr(STR_READWISE_SYNC_NOW)); });
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + contentHeight / 2,
                      tr(STR_READWISE_NO_DOCUMENTS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalRows(), selectedIndex,
        [this](const int index) -> std::string {
          if (index == 0) {
            return tr(STR_READWISE_SYNC_NOW);
          }
          const readwise::Document* doc = docAt(index - 1);
          return doc != nullptr ? std::string(doc->title) : std::string();
        },
        [this](const int index) -> std::string {
          if (index == 0) {
            return {};
          }
          const readwise::Document* doc = docAt(index - 1);
          if (doc == nullptr) {
            return {};
          }
          return doc->author[0] != '\0' ? std::string(doc->author) : std::string(doc->siteName);
        },
        nullptr,
        [this](const int index) -> std::string {
          if (index == 0) {
            return {};
          }
          const readwise::Document* doc = docAt(index - 1);
          if (doc == nullptr) {
            return {};
          }
          return (doc->flags & readwise::FLAG_HAS_BODY) != 0 ? "" : tr(STR_READWISE_NOT_DOWNLOADED);
        });
  }

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
