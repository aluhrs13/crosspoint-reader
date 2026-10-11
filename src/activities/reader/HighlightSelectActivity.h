#pragma once

#include <Epub/Page.h>
#include <ReadwiseDocument.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"

// Range selection over the current page of a managed article. Touch places the
// start, a drag or the side keys extend it, and Confirm (or a second tap on
// the range) queues the quote. Back or a short Home returns to the page;
// long-press Home leaves to the home screen.
class HighlightSelectActivity final : public Activity {
 public:
  HighlightSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                          int marginLeft, int marginTop, const char* documentId);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
  };

  void extractWords();
  int wordAt(int x, int y) const;
  int closestInRow(uint16_t row, int centerX) const;
  void moveEnd(int delta);
  void moveVertical(int direction);
  bool inRange(int index) const;
  void commit();

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  char documentId[readwise::ID_CAP] = {};

  int fontId = 0;
  int lineHeight = 0;
  std::vector<WordBox> words;
  uint16_t rowCount = 0;
  int anchor = -1;
  int end = -1;
  bool saveOnRelease = false;
  unsigned long lastMoveTime = 0;

  bool popup = false;
  bool popupOk = false;
  unsigned long popupTime = 0;
};
