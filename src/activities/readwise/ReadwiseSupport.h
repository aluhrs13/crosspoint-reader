#pragma once

#include <I18n.h>
#include <ReadwiseApi.h>

#include <string>

// Shared helpers for the Readwise activities: the mapping between document ids
// and their cached-body paths, and the managed-path test the reader dispatch
// uses to route Back to the Readwise library instead of the file browser.

namespace ReadwiseUi {

// /.crosspoint/readwise/bodies/<id>.txt
std::string bodyPathForId(const char* id);
// The directory holding an article's archive and the Epub cache built beside
// it. Deleting this removes every trace of the article in one call.
std::string articleDirForId(const char* id);
std::string articleDirForBodyPath(const std::string& path);

// True when `path` is a cached Readwise body, meaning the document must be
// opened managed (library-owned title, Back to the library, no recents entry).
bool isBodyPath(const std::string& path);

// Extracts the document id from a body path; empty when not a body path.
std::string idFromBodyPath(const std::string& path);

// Looks the document up in docs.bin and returns its title, or the id itself
// when the metadata is gone (e.g. the cache was cleared under us).
std::string titleForBodyPath(const std::string& path);

// Translation key for a client status. All user-facing failure text goes
// through this; readwise::apiStatusName() stays log-only English.
StrId statusStrId(readwise::ApiStatus status);

// The long-press menu on a library row, on the preamble, and on the end of an
// article. Author is omitted when `author` is empty: the row then shows the
// site, and filtering on the site is a different action.
enum class ReadwiseEntryAction : uint8_t { Archive = 0, Delete, Comment, Author };

uint8_t fillReadwiseEntryMenu(const char* author, const char** labels, ReadwiseEntryAction* actions, uint8_t cap);

// Hand-off from the reader to the next library. The reader is replaced, so it
// cannot run the shelf action itself. Consumed once, on the next library enter.
struct ShelfReturn {
  enum class Action : uint8_t { None = 0, Archive, Delete, FilterAuthor };
  Action action = Action::None;
  char id[readwise::ID_CAP] = {};
  char author[readwise::AUTHOR_CAP] = {};
};

void queueShelfReturn(const ShelfReturn& request);
ShelfReturn takeShelfReturn();

}  // namespace ReadwiseUi
