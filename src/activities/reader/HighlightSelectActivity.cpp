#include "HighlightSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <NullReadwiseApi.h>
#include <ReadwiseHighlight.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>

#include <climits>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "HighlightWords.h"
#include "ReadwiseCredentialStore.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long POPUP_MS = 900;
constexpr unsigned long REPEAT_START_MS = 500;
constexpr unsigned long REPEAT_INTERVAL_MS = 500;

}  // namespace

HighlightSelectActivity::HighlightSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                                 const char* documentId)
    : Activity("HighlightSelect", renderer, mappedInput),
      page(std::move(page)),
      marginLeft(marginLeft),
      marginTop(marginTop) {
  readwise::copyBounded(this->documentId, sizeof(this->documentId), documentId,
                        documentId != nullptr ? strlen(documentId) : 0);
}

void HighlightSelectActivity::onEnter() {
  Activity::onEnter();
  // Back and Home leave this cancelled. A saved quote replaces it before finish().
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  extractWords();
  requestUpdate();
}

void HighlightSelectActivity::extractWords() {
  words.clear();
  words.reserve(128);
  rowCount = 0;
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto* block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    const int ascender = renderer.getFontAscenderSize(fontId);
    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;
      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      box.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
      box.style = block->wordStyle(i);
      box.width = 0;
      box.row = rowCount;
      box.text = text;
      words.push_back(box);
      rowHasWords = true;
      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    if (rowHasWords) rowCount++;
  }

  if (styleMask == 0) styleMask = 0x01;
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.text, word.style));
  }
}

int HighlightSelectActivity::wordAt(const int x, const int y) const {
  constexpr int kSlop = 4;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x >= word.x - kSlop && x < word.x + word.width + kSlop && y >= word.y - kSlop &&
        y < word.y + lineHeight + kSlop) {
      return i;
    }
  }
  return -1;
}

int HighlightSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

bool HighlightSelectActivity::inRange(const int index) const {
  if (anchor < 0 || end < 0) return false;
  const int lo = anchor < end ? anchor : end;
  const int hi = anchor < end ? end : anchor;
  return index >= lo && index <= hi;
}

void HighlightSelectActivity::moveEnd(const int delta) {
  if (words.empty() || delta == 0) return;
  if (anchor < 0) {
    anchor = static_cast<int>(words.size()) / 2;
    end = anchor;
  }
  const int next = end + delta;
  if (next < 0 || next >= static_cast<int>(words.size())) return;
  end = next;
  requestUpdate();
}

void HighlightSelectActivity::moveVertical(const int direction) {
  if (words.empty()) return;
  if (anchor < 0) {
    anchor = static_cast<int>(words.size()) / 2;
    end = anchor;
  }
  const WordBox& current = words[end];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;
  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != end) {
    end = best;
    requestUpdate();
  }
}

void HighlightSelectActivity::commit() {
  if (anchor < 0 || end < 0) return;
  const int lo = anchor < end ? anchor : end;
  const int hi = anchor < end ? end : anchor;
  auto quote = makeUniqueNoThrow<char[]>(readwise::HIGHLIGHT_TEXT_MAX + 1);
  if (!quote) {
    LOG_ERR("HL", "OOM: quote");
    popup = true;
    popupOk = false;
    popupTime = millis();
    requestUpdate();
    return;
  }
  size_t used = 0;
  quote[0] = '\0';
  for (int i = lo; i <= hi; ++i) {
    const char* word = words[i].text;
    const size_t len = strlen(word);
    if (i > lo && used + 1 < readwise::HIGHLIGHT_TEXT_MAX) {
      quote[used++] = ' ';
    }
    const size_t room = readwise::HIGHLIGHT_TEXT_MAX - used;
    const size_t copy = len < room ? len : room;
    memcpy(quote.get() + used, word, copy);
    used += copy;
    quote[used] = '\0';
    if (used >= readwise::HIGHLIGHT_TEXT_MAX) break;
  }
  readwise::fitHighlightText(quote.get(), readwise::HIGHLIGHT_TEXT_MAX + 1);

  readwise::NullReadwiseApi nullApi;
  readwise::SdReadwiseFileStore store;
  auto engine = makeUniqueNoThrow<readwise::ReadwiseSyncEngine>(nullApi, store, ReadwiseCredentialStore::getDataDir());
  if (engine && engine->appendHighlight(documentId, quote.get())) {
    ActivityResult saved;
    setResult(std::move(saved));
    finish();
    return;
  }
  LOG_ERR("HL", "Could not queue highlight for %s", documentId);
  popup = true;
  popupOk = false;
  popupTime = millis();
  requestUpdate();
}

bool HighlightSelectActivity::handleHomeGesture() {
  if (!popup) finish();
  return true;
}

void HighlightSelectActivity::loop() {
  if (popup) {
    if (millis() - popupTime >= POPUP_MS) finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    commit();
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) {
    const int hit = wordAt(x, y);
    saveOnRelease = hit >= 0 && inRange(hit);
    if (hit >= 0 && !saveOnRelease) {
      anchor = hit;
      end = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.isScreenTouchHeld(x, y) && anchor >= 0) {
    const int hit = wordAt(x, y);
    if (hit >= 0 && hit != end) {
      end = hit;
      saveOnRelease = false;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTouchReleased() && saveOnRelease) {
    saveOnRelease = false;
    haptic_feedback::touchAction();
    commit();
    return;
  }

  if (words.empty()) return;
  const unsigned long now = millis();
  const bool repeat = mappedInput.getHeldTime() >= REPEAT_START_MS && now - lastMoveTime >= REPEAT_INTERVAL_MS;
  const bool left = mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                    (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool right = mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) ||
                     (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenRight));
  if (left || right) {
    lastMoveTime = now;
    moveEnd(left ? -1 : 1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

void HighlightSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto* cache = renderer.getFontCacheManager();
  auto scope = cache->createPrewarmScope();
  page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (anchor >= 0 && end >= 0) {
    const int lo = anchor < end ? anchor : end;
    const int hi = anchor < end ? end : anchor;
    for (int i = lo; i <= hi; ++i) {
      const WordBox& word = words[i];
      renderer.fillRect(word.x - 2, word.y - 2, word.width + 4, lineHeight + 4, true);
      cache->prewarmCache(fontId, word.text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(word.style) & 0x03)));
      renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
    }
  }

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_READWISE_HIGHLIGHT), tr(STR_DIR_LEFT),
                                                         tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (popup) {
    GUI.drawPopup(renderer, popupOk ? tr(STR_READWISE_CLIP_QUEUED) : tr(STR_READWISE_CLIP_FAILED));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
