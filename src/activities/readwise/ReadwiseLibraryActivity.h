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
  void queueArchive(const readwise::Document& doc);
  void switchLocation(int delta);
  int totalRows() const { return 1 + docCount; }

  readwise::NullReadwiseApi nullApi;
  readwise::SdReadwiseFileStore store;
  std::unique_ptr<readwise::ReadwiseSyncEngine> engine;

  ButtonNavigator buttonNavigator;
  State state = State::LIST;

  // The synced locations the user can flip between with Left/Right.
  static constexpr readwise::Location LOCATIONS[] = {readwise::Location::Later, readwise::Location::New};
  int locationIndex = 0;
  uint16_t docCount = 0;

  int selectedIndex = 0;

  // Sliding metadata window backing the visible rows.
  std::vector<readwise::Document> window;
  int windowStart = 0;

  // Set when a download is pending/failed; the id of the document involved.
  std::string pendingDownloadId;
  std::string pendingDownloadTitle;
  std::string statusMessage;
  bool wifiActivated = false;
};
