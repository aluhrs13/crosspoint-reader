#include "ReadwisePreambleActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <NullReadwiseApi.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>

#include <cstdio>
#include <utility>

#include "ReadwiseCredentialStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

constexpr unsigned long HOLD_ACTION_MS = 1000;

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

ReadwisePreambleActivity::ReadwisePreambleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   const char* id)
    : UiListActivity("ReadwisePreamble", renderer, mappedInput) {
  if (id != nullptr) {
    snprintf(documentId, sizeof(documentId), "%s", id);
  }
}

void ReadwisePreambleActivity::onEnter() {
  UiListActivity::onEnter();
  // Anything other than Confirm leaves the article closed. A home gesture that
  // pops this screen must not look like the reader asked to open it.
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));

  // Short-lived engine: findDocument is a docs.bin scan, and the library's
  // own engine stays on the activity underneath this one. Destroy this copy
  // before returning so two readers are not held for the life of the page.
  readwise::NullReadwiseApi api;
  readwise::SdReadwiseFileStore store;
  auto engine = makeUniqueNoThrow<readwise::ReadwiseSyncEngine>(api, store, ReadwiseCredentialStore::getDataDir());
  if (!engine || !engine->findDocument(documentId, doc)) {
    LOG_ERR("RWPRE", "Document not in cache");
    loaded = false;
  } else {
    loaded = true;
    snprintf(wordsBuf, sizeof(wordsBuf), "%lu", static_cast<unsigned long>(doc.wordCount));
    snprintf(progressBuf, sizeof(progressBuf), "%u%%", static_cast<unsigned>(doc.readingProgressPercent));
    engine->readNote(documentId, noteBuf, sizeof(noteBuf));
    rebuildRows();
  }
  engine.reset();

  app.on(ACTION_OPEN, &ReadwisePreambleActivity::openTrampoline, this);
  requestUpdate();
}

void ReadwisePreambleActivity::rebuildRows() {
  rowCount = 0;
  const auto add = [this](const Row row) {
    if (rowCount >= sizeof(rows)) {
      return;
    }
    rows[rowCount++] = static_cast<uint8_t>(row);
  };
  add(Row::Title);
  if (doc.author[0] != '\0') add(Row::Author);
  if (doc.siteName[0] != '\0') add(Row::Site);
  if (doc.sourceUrl[0] != '\0') add(Row::Source);
  if (doc.summary[0] != '\0') add(Row::Summary);
  add(Row::Words);
  add(Row::Category);
  add(Row::Progress);
  if (doc.updatedAt[0] != '\0') add(Row::Updated);
  if (doc.lastMovedAt[0] != '\0') add(Row::Moved);
  add(Row::Downloaded);
  if (noteBuf[0] != '\0') add(Row::Note);
}

void ReadwisePreambleActivity::openTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<ReadwisePreambleActivity*>(user)->confirmOpen();
}

void ReadwisePreambleActivity::confirmOpen() {
  if (!loaded) {
    return;
  }
  app.clearTapFlash();
  setResult(ActivityResult{});
  finish();
}

void ReadwisePreambleActivity::cancel() { finish(); }

bool ReadwisePreambleActivity::handleCustomInput() {
  if (swallowConfirmRelease) {
    const bool released = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool held = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (released || !held) {
      swallowConfirmRelease = false;
    }
    if (held || released) {
      return true;
    }
  }
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    return true;
  }
  int x = 0;
  int y = 0;
  if (loaded && mappedInput.wasScreenLongPress(x, y)) {
    showEntryMenu();
    return true;
  }
  return false;
}

bool ReadwisePreambleActivity::handleHomeGesture() {
  if (!loaded) {
    return false;
  }
  if (!optionPopup.isActive()) {
    showEntryMenu();
  }
  return true;
}

void ReadwisePreambleActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) {
    return;
  }
  UiListActivity::render(std::move(lock));
}

bool ReadwisePreambleActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return true;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    if (loaded && !holdActionTriggered && mappedInput.getHeldTime() >= HOLD_ACTION_MS) {
      holdActionTriggered = true;
      swallowConfirmRelease = true;
      showEntryMenu();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (holdActionTriggered) {
      holdActionTriggered = false;
      return true;
    }
    confirmOpen();
    return true;
  }
  return false;
}

void ReadwisePreambleActivity::navigateButtons() {
  if (!loaded) {
    return;
  }
  // Index 0 is the Open/Download button above the metadata. The list rows
  // follow, so Up from the first field returns to that button.
  const int count = static_cast<int>(rowCount) + 1;
  buttonNavigator.onNextPress([this, count] { moveSelectionTo(ButtonNavigator::nextIndex(nav.selected, count)); });
  buttonNavigator.onPreviousPress(
      [this, count] { moveSelectionTo(ButtonNavigator::previousIndex(nav.selected, count)); });
  buttonNavigator.onNextContinuous(
      [this, count] { moveSelectionTo(ButtonNavigator::nextPageIndex(nav.selected, count, nav.inputPageRows())); });
  buttonNavigator.onPreviousContinuous(
      [this, count] { moveSelectionTo(ButtonNavigator::previousPageIndex(nav.selected, count, nav.inputPageRows())); });
}

const char* ReadwisePreambleActivity::headerTitle() const {
  if (loaded && doc.title[0] != '\0') {
    return doc.title;
  }
  return tr(STR_READWISE_LIBRARY);
}

void ReadwisePreambleActivity::drawFooter() {
  const char* confirmLabel = nullptr;
  if (loaded) {
    confirmLabel = (doc.flags & readwise::FLAG_HAS_BODY) != 0 ? tr(STR_OPEN) : tr(STR_DOWNLOAD);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ReadwisePreambleActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<ReadwisePreambleActivity*>(ctx);
  if (index >= self->rowCount) {
    return;
  }
  const auto row = static_cast<Row>(self->rows[index]);
  const readwise::Document& doc = self->doc;
  switch (row) {
    case Row::Title:
      item.label = doc.title;
      return;
    case Row::Author:
      item.label = tr(STR_READWISE_AUTHOR);
      item.subtitle = doc.author;
      return;
    case Row::Site:
      item.label = tr(STR_READWISE_SITE);
      item.subtitle = doc.siteName;
      return;
    case Row::Source:
      item.label = tr(STR_READWISE_SOURCE);
      item.subtitle = doc.sourceUrl;
      return;
    case Row::Summary:
      item.label = tr(STR_READWISE_SUMMARY);
      item.subtitle = doc.summary;
      return;
    case Row::Words:
      item.label = tr(STR_READWISE_WORDS);
      item.subtitle = self->wordsBuf;
      return;
    case Row::Category:
      item.label = tr(STR_READWISE_CATEGORY);
      item.subtitle = categoryLabel(doc.category);
      return;
    case Row::Progress:
      item.label = tr(STR_READWISE_PROGRESS);
      item.subtitle = self->progressBuf;
      return;
    case Row::Updated:
      item.label = tr(STR_READWISE_UPDATED);
      item.subtitle = doc.updatedAt;
      return;
    case Row::Moved:
      item.label = tr(STR_READWISE_MOVED);
      item.subtitle = doc.lastMovedAt;
      return;
    case Row::Downloaded:
      item.label = tr(STR_READWISE_DOWNLOADED);
      item.subtitle = (doc.flags & readwise::FLAG_HAS_BODY) != 0 ? tr(STR_YES) : tr(STR_NO);
      return;
    case Row::Note:
      item.label = tr(STR_READWISE_NOTE);
      item.subtitle = self->noteBuf;
      return;
  }
}

void ReadwisePreambleActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  if (!loaded) {
    screen.centeredText(tr(STR_READWISE_UNAVAILABLE));
    return;
  }

  const char* openLabel = (doc.flags & readwise::FLAG_HAS_BODY) != 0 ? tr(STR_OPEN) : tr(STR_DOWNLOAD);
  // nav 0 is this button (see navigateButtons). The metadata list starts at 1.
  screen.button(openLabel, ACTION_OPEN, 0, nav.selected == 0 ? fui::StateSelected : fui::StateNormal);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.rowProvider = &ReadwisePreambleActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = rowCount;
  // Display only. Confirm and the Open button leave this screen; a tap on a
  // field must not look like it failed to open the article.
  props.action = fui::NO_ACTION;
  fui::TextStyle label = screen.theme().smallText;
  label.maxLines = 4;
  props.labelText = label;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 8;
  syncListViewport(screen, props, 1);
  screen.list(props);
}

void ReadwisePreambleActivity::finishWith(const ReadwisePreambleResult::Action action) {
  ReadwisePreambleResult shelf;
  shelf.action = action;
  if (action == ReadwisePreambleResult::Action::FilterAuthor) {
    snprintf(shelf.author, sizeof(shelf.author), "%s", doc.author);
  }
  setResult(ActivityResult{std::move(shelf)});
  finish();
}

void ReadwisePreambleActivity::loadNote() {
  noteBuf[0] = '\0';
  readwise::NullReadwiseApi api;
  readwise::SdReadwiseFileStore store;
  auto engine = makeUniqueNoThrow<readwise::ReadwiseSyncEngine>(api, store, ReadwiseCredentialStore::getDataDir());
  if (engine) {
    engine->readNote(documentId, noteBuf, sizeof(noteBuf));
  }
}

void ReadwisePreambleActivity::showEntryMenu() {
  if (!loaded || optionPopup.isActive()) {
    return;
  }
  const char* labels[4] = {};
  menuActionCount = ReadwiseUi::fillReadwiseEntryMenu(doc.author, labels, menuActions, 4);
  if (menuActionCount == 0) {
    return;
  }
  const char* title = doc.title[0] != '\0' ? doc.title : tr(STR_READWISE_LIBRARY);
  optionPopup.show(title, labels, menuActionCount, 0, [this](const int selected) {
    if (selected < 0 || selected >= menuActionCount) {
      return;
    }
    switch (menuActions[selected]) {
      case ReadwiseUi::ReadwiseEntryAction::Archive:
        finishWith(ReadwisePreambleResult::Action::Archive);
        break;
      case ReadwiseUi::ReadwiseEntryAction::Delete: {
        const std::string heading = std::string(tr(STR_DELETE)) + "? ";
        auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, doc.title);
        if (!confirmation) {
          LOG_ERR("RWPRE", "OOM: delete confirmation");
          return;
        }
        startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            finishWith(ReadwisePreambleResult::Action::Delete);
          }
        });
        break;
      }
      case ReadwiseUi::ReadwiseEntryAction::Comment:
        startComment();
        break;
      case ReadwiseUi::ReadwiseEntryAction::Author:
        finishWith(ReadwisePreambleResult::Action::FilterAuthor);
        break;
    }
  });
  requestUpdate();
}

void ReadwisePreambleActivity::startComment() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READWISE_COMMENT), noteBuf,
                                                           readwise::ReadwiseSyncEngine::NOTE_CAP - 1);
  if (!keyboard) {
    LOG_ERR("RWPRE", "OOM: comment keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      return;
    }
    const auto* entered = std::get_if<KeyboardResult>(&result.data);
    if (entered == nullptr) {
      return;
    }
    readwise::NullReadwiseApi api;
    readwise::SdReadwiseFileStore store;
    auto engine = makeUniqueNoThrow<readwise::ReadwiseSyncEngine>(api, store, ReadwiseCredentialStore::getDataDir());
    if (!engine || !engine->writeNote(documentId, entered->text.c_str())) {
      LOG_ERR("RWPRE", "Could not save note");
      return;
    }
    loadNote();
    rebuildRows();
    requestUpdate();
  });
}
