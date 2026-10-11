#pragma once

#include <NullReadwiseApi.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/UiTabListActivity.h"
#include "activities/readwise/ReadwiseSupport.h"
#include "components/OptionPopup.h"

/**
 * Offline browser for the synced Readwise locations.
 *
 * Reads only the local index and metadata files -- no network. Later,
 * Shortlist, and Feed are shelves. Long Reads and Quick Reads are views of
 * Later and Shortlist split by word count. Each tab keeps its own selection
 * and scroll.
 * Row 0 is "Sync now" (which pushes ReadwiseSyncActivity). A document row
 * opens a preamble; Confirm there opens the cached body or downloads it.
 * A long press opens the entry menu. Holding Left or Right still sends the
 * selected article to another shelf.
 *
 * Only one visible window of metadata is resident at a time: documents are
 * ~800 bytes each and a full 100-document cap would be ~80 KB.
 */
class ReadwiseLibraryActivity final : public UiTabListActivity {
 public:
  explicit ReadwiseLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiTabListActivity("ReadwiseLibrary", renderer, mappedInput, true) {}

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
#ifdef CROSSPOINT_READWISE_ONLY
  // This library is the home screen in the Readwise-only build, so the home
  // gesture must not try to navigate to a home behind it -- that would replace
  // this activity with another copy of itself.
  bool isHomeActivity() const override { return true; }
#endif

 private:
  enum class State : uint8_t {
    LIST,
    DOWNLOADING,
    DOWNLOAD_FAILED,
    DELETING,
    NOTICE,
  };

  int listCount() const override { return totalRows(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override;
  void drawFooter() override;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  int tabCount() const override { return queuedCount > 0 ? TAB_COUNT + 1 : TAB_COUNT; }
  int activeTab() const override { return locationIndex; }
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;

  void reloadCounts();
  void ensureWindow(int docIndex);
  const readwise::Document* docAt(int docIndex);
  void openDocument(const readwise::Document& doc);
  void startDownload(const readwise::Document& doc);
  void performDownload();
  void migrateLegacyTextBodies();
  static void sImageProgress(void* ctx, size_t done, size_t total);
  void queueMove(const readwise::Document& doc, readwise::Location target);
  void commitLocationChange(const char* id, readwise::Location target, const char* remoteRev);
  // Applies a shelf action queued by the reader at the end of an article.
  void applyShelfReturn();
  void showEntryMenu(int index);
  void confirmDelete();
  void pushDelete();
  void startComment();
  void applyAuthorFilter(const char* author);
  void clearAuthorFilter();
  bool authorFilterActive() const { return authorFilter[0] != '\0'; }
  bool lengthTab() const { return locationIndex == LONG_TAB || locationIndex == QUICK_TAB; }
  bool queuedTab() const { return queuedCount > 0 && locationIndex == QUEUED_TAB; }
  void refreshQueued();
  // Long-pressing a location button sends the selected article there instead
  // of switching to that view. Returns true while the button is held, so the
  // caller swallows the frame.
  bool handleLocationHold(MappedInputManager::Button button, int targetIndex);
  void selectTab(int index);
  void jumpToLocation(int index);
  // Persists locationIndex so Back out of an article returns to this view.
  void rememberLocation();
  // List row under the tab band. -1 when the band itself is focused.
  int selectedRow() const { return ringPos() - 1; }
  // Direct-jump targets for the Left/Right buttons: from Later they lead to
  // Shortlist and Feed; from Shortlist or Feed the button for the current view
  // leads back to Later. The hint labels name the destination view.
  int leftTargetIndex() const { return locationIndex == 1 ? 0 : 1; }
  int rightTargetIndex() const { return locationIndex == 2 ? 0 : 2; }
  int totalRows() const { return 1 + docCount; }

  readwise::NullReadwiseApi nullApi;
  readwise::SdReadwiseFileStore store;
  std::unique_ptr<readwise::ReadwiseSyncEngine> engine;

  State state = State::LIST;

  // Shelves the API knows, then two length views that are not locations.
  // Index 0 (Later) is the default. leftTargetIndex and rightTargetIndex
  // stay inside the shelves on boards that have Left/Right.
  static constexpr int LOCATION_COUNT = 3;
  static constexpr int LONG_TAB = 3;
  static constexpr int QUICK_TAB = 4;
  static constexpr int TAB_COUNT = 5;
  // Shown only while a delete or archive is still in the journal.
  static constexpr int QUEUED_TAB = 5;
  // 1,500 words is about six minutes. Zero is on neither length tab.
  static constexpr uint32_t LONG_READ_WORDS = 1500;
  static constexpr readwise::Location LOCATIONS[LOCATION_COUNT] = {
      readwise::Location::Later, readwise::Location::Shortlist, readwise::Location::Feed};
  int locationIndex = 0;
  uint16_t docCount = 0;
  uint16_t queuedCount = 0;
  std::vector<readwise::ReadwiseSyncEngine::QueuedDocument> queued;
  // tabLabel() is called once per tab and the bar keeps the pointer, so the
  // queued label (which includes the count) needs its own buffer.
  mutable char queuedLabel[24] = {};

  // Sliding metadata window backing the visible rows.
  std::vector<readwise::Document> window;
  int windowStart = 0;

  // One scratch string for the row value (word count and date). list() reads
  // the pointer before it asks for the next row.
  char rowValue[32] = {};

  // Set when a download is pending/failed; the id of the document involved.
  std::string pendingDownloadId;
  std::string pendingDownloadTitle;
  std::string pendingDownloadRev;
  // Relative image srcs resolve against source_url, and the OPF wants the
  // author, so both are carried from the row rather than re-read later.
  std::string pendingDownloadSourceUrl;
  std::string pendingDownloadAuthor;
  bool pendingDownloadSeen = false;
  std::string statusMessage;
  // Set when a hold has already acted (entry menu, or a move to another view),
  // so the release that follows does not also open the document or switch views.
  bool holdActionTriggered = false;
  // The Confirm release that opened the menu must not also select Archive.
  bool swallowConfirmRelease = false;
  OptionPopup optionPopup;
  // Copied out of the sliding window before the menu acts. The window can be
  // rebuilt before the confirmation or the keyboard returns.
  char menuId[readwise::ID_CAP] = {};
  char menuTitle[readwise::TITLE_CAP] = {};
  char menuAuthor[readwise::AUTHOR_CAP] = {};
  char menuRev[readwise::TIMESTAMP_CAP] = {};
  ReadwiseUi::ReadwiseEntryAction menuActions[4] = {};
  uint8_t menuActionCount = 0;
  // Empty when the shelf is unfiltered. The header shows this string.
  char authorFilter[readwise::AUTHOR_CAP] = {};
  // Location-index offsets of the matching documents. One allocation when the
  // filter is applied, sized to the shelf, rather than a resident copy of
  // every matching record.
  std::vector<uint16_t> authorSlots;
  // docs.bin record indexes for the active length tab, newest first.
  std::vector<uint16_t> lengthSlots;
  bool wifiActivated = false;
  // Back is also what navigates *into* this screen (from Settings, or out of a
  // managed article), and the button is often still held when the activity is
  // constructed. Without this, the trailing release acts again immediately and
  // bounces straight back out to where the user just came from.
  bool ignoreBackUntilReleased = false;
};
