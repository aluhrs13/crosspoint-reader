#pragma once

#include <string>

// Shared helpers for the Readwise activities: the mapping between document ids
// and their cached-body paths, and the managed-path test the reader dispatch
// uses to route Back to the Readwise library instead of the file browser.

namespace ReadwiseUi {

// /.crosspoint/readwise/bodies/<id>.txt
std::string bodyPathForId(const char* id);

// True when `path` is a cached Readwise body, meaning the document must be
// opened managed (library-owned title, Back to the library, no recents entry).
bool isBodyPath(const std::string& path);

// Extracts the document id from a body path; empty when not a body path.
std::string idFromBodyPath(const std::string& path);

// Looks the document up in docs.bin and returns its title, or the id itself
// when the metadata is gone (e.g. the cache was cleared under us).
std::string titleForBodyPath(const std::string& path);

}  // namespace ReadwiseUi
