#pragma once

#include <ReadwiseHighlights.h>
#include <Txt.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

// Narrow fork hook for TxtReaderActivity: when `managed` is set, the document
// belongs to the Readwise library. The reader then shows `title` instead of
// the filename, skips the recents entry, and routes Back to the Readwise
// library rather than the file browser/home. Defined at namespace scope
// because GCC rejects a `{}` default argument of a nested aggregate with
// default member initializers (PR96645).
struct TxtManagedDocInfo {
  std::string title;
  bool managed = false;
};

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  // File byte offset of each visual line's first byte, parallel to
  // currentPageLines. A line's byte range is [offset, offset + line.size())
  // because each visual line is byte-identical to the file content it shows.
  std::vector<size_t> currentLineOffsets;
  // Readwise highlights on this document as byte ranges into the body file,
  // loaded once per open; <= 64 spans (8 bytes each). Empty for unmanaged docs.
  std::vector<readwise::SentenceSpan> docHighlights;
  std::string managedDocId;

  // --- highlight selection mode (managed docs only) -----------------------
  // Confirm enters the mode; Up/Down move between sentences on the visible
  // page, Right/Left grow/shrink the selection, Confirm commits, Back cancels.
  bool highlightMode = false;
  int selAnchor = 0;
  int selCount = 1;
  // Sentence spans of the visible page, absolute byte offsets. Transient:
  // populated on entering the mode, freed on exit (~1 KB worst case).
  std::vector<readwise::SentenceSpan> pageSentences;
  // Byte offset one past the visible page's content, from the last render.
  size_t currentPageEnd = 0;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;
  void drawHighlightUnderlines(size_t lineIndex, int lineX, int lineY, int lineHeight);

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                        std::vector<size_t>* outLineOffsets = nullptr);
  void loadDocHighlights();
  void enterHighlightMode();
  void exitHighlightMode();
  void handleHighlightModeInput();
  void commitHighlight();
  size_t selectionStart() const;
  size_t selectionEnd() const;
  void drawSelectionSegment(size_t lineIndex, int lineX, int lineY, int lineHeight);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

  TxtManagedDocInfo managedDoc;

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             int initialRefreshCountdown, TxtManagedDocInfo managedInfo = {})
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown),
        managedDoc(std::move(managedInfo)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
};
