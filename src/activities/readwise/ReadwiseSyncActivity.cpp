#include "ReadwiseSyncActivity.h"

#include <GfxRenderer.h>
#include <HttpReadwiseApi.h>
#include <I18n.h>
#include <Memory.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "ReadwiseCredentialStore.h"
#include "ReadwiseImageFetcher.h"
#include "ReadwiseSupport.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

const char* categoryLabel(const readwise::Category category) {
  switch (category) {
    case readwise::Category::Article:
      return tr(STR_READWISE_CAT_ARTICLE);
    case readwise::Category::Rss:
      return tr(STR_READWISE_CAT_RSS);
    case readwise::Category::Email:
      return tr(STR_READWISE_CAT_EMAIL);
    case readwise::Category::Tweet:
      return tr(STR_READWISE_CAT_TWEET);
    case readwise::Category::Pdf:
      return tr(STR_READWISE_CAT_PDF);
    case readwise::Category::Video:
      return tr(STR_READWISE_CAT_VIDEO);
    case readwise::Category::Highlight:
      return tr(STR_READWISE_CAT_HIGHLIGHT);
    case readwise::Category::Note:
      return tr(STR_READWISE_CAT_NOTE);
    case readwise::Category::Epub:
      return tr(STR_READWISE_CAT_EPUB);
    case readwise::Category::Unknown:
      break;
  }
  return tr(STR_READWISE_CAT_UNKNOWN);
}

}  // namespace

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
  // The sync blocked the main loop for its whole duration; without this the
  // inactivity timer is already past the timeout and the result screen would
  // be replaced by the sleep screen immediately.
  activityManager.noteBlockingWorkFinished();
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
  readwise::ReadwiseSyncEngine::NotFoundHooks notFound;
  notFound.ctx = this;
  notFound.onNotFound = &ReadwiseSyncActivity::onNotFound;
  engine->setNotFoundHooks(notFound);

  const readwise::SyncOutcome outcome = engine->sync();

  if (outcome.ok) {
    // Sync is the only online operation: fetch every missing article body now
    // so the whole library reads offline afterwards. Throttling is expected --
    // body fetches share the list endpoint's 20 req/min budget -- and the
    // engine waits out retry-after between documents via the sleep hook.
    {
      RenderLock lock(*this);
      state = State::DOWNLOADING_BODIES;
      pushed = outcome.pushed;
      pulled = outcome.pulled;
      highlightsSent = outcome.highlightsSent;
      highlightsFailed = outcome.highlightsFailed;
      readwise::copyBounded(highlightDetail, sizeof(highlightDetail), outcome.highlightDetail,
                            strlen(outcome.highlightDetail));
    }
    requestUpdateAndWait();

    readwise::ReadwiseSyncEngine::BodySyncHooks hooks;
    hooks.ctx = this;
    hooks.onProgress = [](void* ctx, uint16_t /*done*/, uint16_t total) {
      auto* self = static_cast<ReadwiseSyncActivity*>(ctx);
      bool changed = false;
      {
        RenderLock lock(*self);
        // Finished-article count comes from onStep. This only publishes how
        // many articles there are, before the first one starts.
        changed = self->bodiesTotal != total;
        self->bodiesTotal = total;
      }
      // Immediate: a deferred update only fires when loop() returns, and this
      // whole pass runs blocking inside one loop() iteration.
      if (changed) {
        self->requestUpdate(true);
      }
    };
    hooks.onStep = [](void* ctx, const readwise::ReadwiseSyncEngine::BodySyncProgress& progress) {
      auto* self = static_cast<ReadwiseSyncActivity*>(ctx);
      {
        RenderLock lock(*self);
        self->bodiesDone = progress.articlesDone;
        self->bodiesTotal = progress.articlesTotal;
        self->currentWords = progress.wordCount;
        self->currentCategory = progress.category;
        self->stepIndex = progress.index;
        self->stepCount = progress.count;
        switch (progress.step) {
          case readwise::ReadwiseSyncEngine::BodySyncStep::Article:
            self->downloadStep = DownloadStep::Article;
            break;
          case readwise::ReadwiseSyncEngine::BodySyncStep::Image:
            self->downloadStep = DownloadStep::Image;
            break;
          case readwise::ReadwiseSyncEngine::BodySyncStep::RateLimit:
            self->downloadStep = DownloadStep::RateLimit;
            break;
        }
        snprintf(self->currentTitle, sizeof(self->currentTitle), "%s", progress.title != nullptr ? progress.title : "");
      }
      self->requestUpdate(true);
    };
    hooks.sleepMs = [](void*, uint32_t ms) { delay(ms); };
    // Wi-Fi is already up for the metadata pass, so article images ride along
    // with it and reading stays fully offline afterwards.
    ReadwiseUi::HttpArticleImageFetcher imageFetcher;
    hooks.imageFetcher = &imageFetcher;
    const auto bodies = engine->downloadMissingBodies(hooks);
    engine.reset();

    RenderLock lock(*this);
    bodiesDone = bodies.downloaded;
    bodiesTotal = bodies.total;
    bodiesFailed = bodies.failed;
    failedTitle = bodies.failedTitle;
    if (bodies.failed > 0) {
      LOG_ERR("RWSYNC", "%u body download(s) failed; first: \"%s\" (%s)", (unsigned)bodies.failed, bodies.failedTitle,
              readwise::apiStatusName(bodies.failedStatus));
    }
    if (bodies.ok) {
      state = State::COMPLETE;
    } else {
      // Metadata committed; only the body pass failed. Report the cause; the
      // downloaded articles stay cached and the rest retry on demand.
      state = State::FAILED;
      statusMessage = I18N.get(ReadwiseUi::statusStrId(bodies.status));
      readwise::copyBounded(failureDetail, sizeof(failureDetail), api.lastDetail(), strlen(api.lastDetail()));
    }
    requestUpdate();
    return;
  }
  engine.reset();

  RenderLock lock(*this);
  {
    state = State::FAILED;
    statusMessage = outcome.status == readwise::ApiStatus::Ok ? tr(STR_READWISE_SYNC_FAILED)
                                                              : I18N.get(ReadwiseUi::statusStrId(outcome.status));
    readwise::copyBounded(failureDetail, sizeof(failureDetail), outcome.detail, strlen(outcome.detail));
    highlightsSent = outcome.highlightsSent;
    highlightsFailed = outcome.highlightsFailed;
    readwise::copyBounded(highlightDetail, sizeof(highlightDetail), outcome.highlightDetail,
                          strlen(outcome.highlightDetail));
    LOG_ERR("RWSYNC", "Sync failed: stage=%u status=%s detail=%s", static_cast<unsigned>(outcome.failedStage),
            readwise::apiStatusName(outcome.status), failureDetail);
  }
  requestUpdate();
}

bool ReadwiseSyncActivity::onNotFound(void* ctx, const readwise::PendingOp& op, const char* title, const char* detail) {
  return static_cast<ReadwiseSyncActivity*>(ctx)->promptDropMissing(op.id, title, detail);
}

bool ReadwiseSyncActivity::promptDropMissing(const char* id, const char* title, const char* detail) {
  const char* name = (title != nullptr && title[0] != '\0') ? title : (id != nullptr ? id : "");
  const char* why = tr(STR_READWISE_MAY_BE_GONE);
  if (name[0] != '\0') {
    snprintf(skipMessage, sizeof(skipMessage), "%s. %s", name, why);
  } else {
    snprintf(skipMessage, sizeof(skipMessage), "%s", why);
  }
  LOG_INF("RWSYNC", "Not on server: %s (%s)", name, detail != nullptr ? detail : "");

  const char* options[] = {tr(STR_STOP), tr(STR_CONTINUE)};
  // -1 until a button fires. Back and an outside tap dismiss without choosing,
  // which keeps the queued op and stops the sync.
  int choice = -1;
  // Continue is the default: a missing document should not end the sync, and
  // Confirm on a button device takes the highlighted option.
  skipPopup.showMessage(tr(STR_READWISE_NOT_ON_SERVER), skipMessage, options, 2, 1, [&](int index) { choice = index; });

  {
    RenderLock lock(*this);
    state = State::SKIP_PROMPT;
  }
  // Drop a press that landed while the request was in flight.
  mappedInput.update();
  requestUpdateAndWait();

  while (skipPopup.isActive()) {
    delay(20);
    mappedInput.update();
    skipPopup.handleInput(mappedInput, [this]() {
      if (!skipPopup.isActive()) {
        RenderLock lock(*this);
        state = State::SYNCING;
      }
      requestUpdate(true);
    });
  }
  skipPopup.dismiss();

  const bool drop = choice == 1;
  if (drop) {
    LOG_INF("RWSYNC", "Removing %s and continuing", name);
  } else {
    LOG_INF("RWSYNC", "Stopping sync; keeping %s queued", name);
    RenderLock lock(*this);
    state = State::SYNCING;
  }
  requestUpdate(true);
  return drop;
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

// The completion summary is the one screen with something to say, so it is
// laid out as real lines rather than squeezed into drawPopup, which is a
// single line and does not wrap.
void ReadwiseSyncActivity::renderComplete() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID) + 6;

  // Centre the block: a clean sync is 2 lines, a sync with failures is 4 or 5.
  const bool showSent = highlightsSent > 0;
  const bool showFailed = highlightsFailed > 0;
  int lines = bodiesTotal > 0 ? 2 : 1;
  if (showSent) lines++;
  if (showFailed) lines++;
  if (showFailed && highlightDetail[0] != '\0') lines++;
  if (bodiesFailed > 0) {
    lines += failedTitle.empty() ? 1 : 2;
    if (bodiesFailed > 1) lines++;
  }
  int y = pageHeight / 2 - (lines * lineHeight) / 2;

  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_READWISE_SYNC_COMPLETE), true, EpdFontFamily::BOLD);
  y += lineHeight;

  if (showSent || showFailed) {
    char line[80];
    if (showSent) {
      snprintf(line, sizeof(line), "%u %s", static_cast<unsigned>(highlightsSent),
               highlightsSent == 1 ? tr(STR_READWISE_HIGHLIGHT_SENT) : tr(STR_READWISE_HIGHLIGHTS_SENT));
      renderer.drawCenteredText(UI_10_FONT_ID, y, line, true);
      y += lineHeight;
    }
    if (showFailed) {
      snprintf(line, sizeof(line), "%u %s", static_cast<unsigned>(highlightsFailed),
               highlightsFailed == 1 ? tr(STR_READWISE_HIGHLIGHT_FAILED) : tr(STR_READWISE_HIGHLIGHTS_FAILED));
      renderer.drawCenteredText(UI_10_FONT_ID, y, line, true);
      y += lineHeight;
    }
    if (showFailed && highlightDetail[0] != '\0') {
      const int available = pageWidth - metrics.statusBarHorizontalMargin * 4;
      const char* shown = highlightDetail;
      std::string truncated;
      if (available > 0 && renderer.getTextWidth(UI_10_FONT_ID, highlightDetail) > available) {
        truncated = renderer.truncatedText(UI_10_FONT_ID, highlightDetail, available);
        shown = truncated.c_str();
      }
      renderer.drawCenteredText(UI_10_FONT_ID, y, shown, true);
      y += lineHeight;
    }
  }

  if (bodiesTotal > 0) {
    const std::string counts = std::to_string(bodiesDone) + "/" + std::to_string(bodiesTotal) + " " +
                               std::string(tr(STR_READWISE_ARTICLES_DOWNLOADED));
    renderer.drawCenteredText(UI_10_FONT_ID, y, counts.c_str(), true);
    y += lineHeight;
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_READWISE_NOTHING_TO_DOWNLOAD), true);
    y += lineHeight;
  }

  if (bodiesFailed > 0) {
    y += 6;  // a little air before the failure block
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_READWISE_COULD_NOT_DOWNLOAD), true);
    y += lineHeight;

    if (!failedTitle.empty()) {
      // Titles are up to 128 chars; keep the line inside the screen margins.
      const int available = pageWidth - metrics.statusBarHorizontalMargin * 4;
      std::string shown = failedTitle;
      if (renderer.getTextWidth(UI_10_FONT_ID, shown.c_str()) > available) {
        shown = renderer.truncatedText(UI_10_FONT_ID, shown.c_str(), available);
      }
      // The UI font family carries regular and bold only, so bold is what
      // distinguishes the title from the label above it.
      renderer.drawCenteredText(UI_10_FONT_ID, y, shown.c_str(), true, EpdFontFamily::BOLD);
      y += lineHeight;
    }

    if (bodiesFailed > 1) {
      const std::string more =
          "+" + std::to_string(bodiesFailed - 1) + " " + std::string(tr(STR_READWISE_AND_MORE_FAILED));
      renderer.drawCenteredText(UI_10_FONT_ID, y, more.c_str(), true);
    }
  }
}

// Article count, the article being fetched, and either its category and word
// count or which of its images is downloading. drawPopup is one line and does
// not wrap, which hid all of that.
void ReadwiseSyncActivity::renderDownloading() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int gap = 6;
  // BaseTheme::drawProgressBar paints its percent label this far below the bar.
  constexpr int kPercentOffset = 15;

  const int top = metrics.topPadding + metrics.headerHeight;
  const int bottom = pageHeight - metrics.buttonHintsHeight;
  const bool haveTotal = bodiesTotal > 0;

  int block = lineHeight;  // heading
  if (haveTotal) {
    block += gap + lineHeight;  // count
    block += gap + metrics.progressBarHeight + kPercentOffset + lineHeight;
  }
  block += gap + lineHeight + gap + lineHeight;  // title, detail
  int y = top + (bottom - top - block) / 2;
  if (y < top) {
    y = top;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_READWISE_DOWNLOADING), true, EpdFontFamily::BOLD);
  y += lineHeight + gap;

  if (haveTotal) {
    unsigned working = static_cast<unsigned>(bodiesDone) + 1;
    if (working > bodiesTotal) {
      working = bodiesTotal;
    }
    char counts[24];
    snprintf(counts, sizeof(counts), "%u / %u", working, static_cast<unsigned>(bodiesTotal));
    renderer.drawCenteredText(UI_10_FONT_ID, y, counts, true);
    y += lineHeight + gap;

    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        bodiesDone, bodiesTotal);
    y += metrics.progressBarHeight + kPercentOffset + lineHeight + gap;
  }

  const int available = pageWidth - metrics.contentSidePadding * 2;
  if (currentTitle[0] != '\0' && available > 0) {
    const char* shown = currentTitle;
    std::string truncated;
    if (renderer.getTextWidth(UI_10_FONT_ID, currentTitle, EpdFontFamily::BOLD) > available) {
      truncated = renderer.truncatedText(UI_10_FONT_ID, currentTitle, available, EpdFontFamily::BOLD);
      shown = truncated.c_str();
    }
    renderer.drawCenteredText(UI_10_FONT_ID, y, shown, true, EpdFontFamily::BOLD);
  }
  y += lineHeight + gap;

  char detail[96];
  detail[0] = '\0';
  switch (downloadStep) {
    case DownloadStep::Image:
      if (stepCount > 0) {
        unsigned shown = stepIndex;
        if (shown < stepCount) {
          ++shown;
        }
        snprintf(detail, sizeof(detail), "%s %u/%u", tr(STR_IMAGES), shown, static_cast<unsigned>(stepCount));
      }
      break;
    case DownloadStep::RateLimit:
      snprintf(detail, sizeof(detail), "%s", tr(STR_READWISE_RATE_LIMITED));
      break;
    case DownloadStep::Article: {
      const bool named = currentCategory != readwise::Category::Unknown;
      if (named && currentWords > 0) {
        snprintf(detail, sizeof(detail), "%s · %lu %s", categoryLabel(currentCategory),
                 static_cast<unsigned long>(currentWords), tr(STR_READWISE_WORDS));
      } else if (named) {
        snprintf(detail, sizeof(detail), "%s", categoryLabel(currentCategory));
      } else if (currentWords > 0) {
        snprintf(detail, sizeof(detail), "%lu %s", static_cast<unsigned long>(currentWords), tr(STR_READWISE_WORDS));
      }
      break;
    }
  }
  if (detail[0] == '\0' || available <= 0) {
    return;
  }
  const char* shown = detail;
  std::string truncated;
  if (renderer.getTextWidth(UI_10_FONT_ID, detail) > available) {
    truncated = renderer.truncatedText(UI_10_FONT_ID, detail, available);
    shown = truncated.c_str();
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, shown, true);
}

// The status line is the translated cause. The trace under it names the step
// (highlight, delete, move, pull) and, when the server sent one, the HTTP
// status and a short response snippet. drawPopup is one line and does not wrap.
void ReadwiseSyncActivity::renderFailed() const {
  static_assert(FAILURE_DETAIL_CAP == sizeof(readwise::SyncOutcome::detail),
                "failure trace must fit the engine's detail");
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  const int available = pageWidth - metrics.contentSidePadding * 2;
  const int x = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight;
  const int bottom = pageHeight - metrics.buttonHintsHeight;

  const auto lines = failureDetail[0] != '\0' && available > 0
                         ? renderer.wrappedText(UI_10_FONT_ID, failureDetail, available, 8)
                         : std::vector<std::string>{};
  char highlightLine[80];
  highlightLine[0] = '\0';
  if (highlightsSent > 0 || highlightsFailed > 0) {
    size_t used = 0;
    if (highlightsSent > 0) {
      const int wrote =
          snprintf(highlightLine, sizeof(highlightLine), "%u %s", static_cast<unsigned>(highlightsSent),
                   highlightsSent == 1 ? tr(STR_READWISE_HIGHLIGHT_SENT) : tr(STR_READWISE_HIGHLIGHTS_SENT));
      if (wrote > 0) {
        used = static_cast<size_t>(wrote);
      }
    }
    if (highlightsFailed > 0 && used + 1 < sizeof(highlightLine)) {
      snprintf(highlightLine + used, sizeof(highlightLine) - used, "%s%u %s", used > 0 ? ", " : "",
               static_cast<unsigned>(highlightsFailed),
               highlightsFailed == 1 ? tr(STR_READWISE_HIGHLIGHT_FAILED) : tr(STR_READWISE_HIGHLIGHTS_FAILED));
    }
  }
  const int gap = lines.empty() ? 0 : 8;
  const int highlightRows = highlightLine[0] != '\0' ? 1 : 0;
  const int block = lineHeight + highlightRows * lineHeight + gap + static_cast<int>(lines.size()) * lineHeight;
  int y = top + (bottom - top - block) / 2;
  if (y < top) {
    y = top;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, y, statusMessage.c_str(), true, EpdFontFamily::BOLD);
  y += lineHeight;
  if (highlightLine[0] != '\0') {
    renderer.drawCenteredText(UI_10_FONT_ID, y, highlightLine, true);
    y += lineHeight;
  }
  y += gap;
  for (const auto& line : lines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
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
    case State::SKIP_PROMPT:
      // The dialog paints itself, including its own button hints, and presents.
      // X4 Pro is touch, so those hints are blank and the buttons are on screen.
      if (skipPopup.processRender(renderer, mappedInput)) {
        return;
      }
      break;
    case State::DOWNLOADING_BODIES:
      renderDownloading();
      break;
    case State::COMPLETE:
      renderComplete();
      break;
    case State::FAILED:
      renderFailed();
      break;
    case State::NO_TOKEN:
      GUI.drawPopup(renderer, tr(STR_READWISE_SET_TOKEN_FIRST));
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
