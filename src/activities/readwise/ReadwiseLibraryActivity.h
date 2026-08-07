#pragma once

#include <NullReadwiseApi.h>
#include <ReadwiseSyncEngine.h>
#include <SdReadwiseFileStore.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Offline browser for the synced Readwise locations.
 *
 * Reads only the local index and metadata files -- no network. Row 0 is the
 * "Sync now" entry (which pushes ReadwiseSyncActivity); document rows open the
 * cached body, or download it on demand when absent. A long Confirm press
 * queues an archive, which takes effect locally immediately via
 * rebuildLocal().
 *
 * Only one visible window of metadata is resident at a time: documents are
 * ~800 bytes each and a full 100-document cap would be ~80 KB.
 */
class ReadwiseLibraryActivity final : public Activity {
 public:
  explicit ReadwiseLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadwiseLibrary", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
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
  };

  void reloadCounts();
  void ensureWindow(int docIndex);
  const readwise::Document* docAt(int docIndex);
  void activateSelection();
  void openDocument(const readwise::Document& doc);
  void startDownload(const readwise::Document& doc);
  void performDownload();
  void migrateLegacyTextBodies();
  static void sImageProgress(void* ctx, size_t done, size_t total);
  void queueArchive(const readwise::Document& doc);
  void jumpToLocation(int index);
  // Persists locationIndex so Back out of an article returns to this view.
  void rememberLocation();
  // Direct-jump targets for the Left/Right buttons: from Later they lead to
  // Shortlist and Feed; from Shortlist or Feed the button for the current view
  // leads back to Later. The hint labels name the destination view.
  int leftTargetIndex() const { return locationIndex == 1 ? 0 : 1; }
  int rightTargetIndex() const { return locationIndex == 2 ? 0 : 2; }
  int totalRows() const { return 1 + docCount; }

  readwise::NullReadwiseApi nullApi;
  readwise::SdReadwiseFileStore store;
  std::unique_ptr<readwise::ReadwiseSyncEngine> engine;

  ButtonNavigator buttonNavigator;
  State state = State::LIST;

  // The library views, jumped between with Left/Right. Index 0 (Later) is the
  // default; leftTargetIndex/rightTargetIndex encode the button mapping.
  static constexpr readwise::Location LOCATIONS[] = {readwise::Location::Later, readwise::Location::Shortlist,
                                                     readwise::Location::Feed};
  int locationIndex = 0;
  uint16_t docCount = 0;

  int selectedIndex = 0;

  // Sliding metadata window backing the visible rows.
  std::vector<readwise::Document> window;
  int windowStart = 0;

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
  // Set when a hold has already archived, so the following Confirm release
  // does not also open the document.
  bool archiveTriggered = false;
  bool wifiActivated = false;
  // Back is also what navigates *into* this screen (from Settings, or out of a
  // managed article), and the button is often still held when the activity is
  // constructed. Without this, the trailing release acts again immediately and
  // bounces straight back out to where the user just came from.
  bool ignoreBackUntilReleased = false;
};
