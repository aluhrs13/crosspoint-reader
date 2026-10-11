#pragma once

#include <ReadwiseDocument.h>
#include <ReadwiseSyncEngine.h>

#include "activities/UiListActivity.h"
#include "activities/readwise/ReadwiseSupport.h"
#include "components/OptionPopup.h"

/**
 * Metadata page shown before a Readwise article opens.
 *
 * Holds one Document (~800 bytes, too large for the stack) loaded by id from
 * docs.bin. Confirm, or the Open/Download button, finishes with a result so
 * the library runs the existing open-or-download path. Back cancels.
 * Home, a touch long-press, or a held Confirm opens the same entry menu as
 * the library. Archive and delete return to the shelf. A comment stays here.
 */
class ReadwisePreambleActivity final : public UiListActivity {
 public:
  ReadwisePreambleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* id);

  void onEnter() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 protected:
  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  const char* headerTitle() const override;
  void drawFooter() override;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

 private:
  enum class Row : uint8_t {
    Title,
    Author,
    Site,
    Source,
    Summary,
    Words,
    Category,
    Progress,
    Updated,
    Moved,
    Downloaded,
    Note,
  };

  static constexpr freeink::ui::ActionId ACTION_OPEN = ACTION_USER;

  void rebuildRows();
  void confirmOpen();
  void cancel();
  void showEntryMenu();
  void startComment();
  void loadNote();
  void finishWith(ReadwisePreambleResult::Action action);
  static void openTrampoline(const freeink::ui::ActionEvent& event, void* user);

  char documentId[readwise::ID_CAP] = {};
  readwise::Document doc;
  char wordsBuf[16] = {};
  char progressBuf[8] = {};
  char noteBuf[readwise::ReadwiseSyncEngine::NOTE_CAP] = {};
  uint8_t rows[12] = {};
  uint8_t rowCount = 0;
  bool loaded = false;
  bool holdActionTriggered = false;
  bool swallowConfirmRelease = false;
  OptionPopup optionPopup;
  ReadwiseUi::ReadwiseEntryAction menuActions[4] = {};
  uint8_t menuActionCount = 0;
};
