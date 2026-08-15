// Probes for how ArticleBodyWriter emits the XHTML prologue.
//
// A device-captured article.epub contained TWO document prologues, the second
// injected mid-body (inside an open <p>), which made the XHTML unparseable.
// These tests pin down which call sequence produces that.

#include <gtest/gtest.h>

#include <string>

#include "FakeReadwise.h"
#include "lib/Readwise/ArticleBodyWriter.h"

using readwise::ArticleBodyWriter;
using testing_support::FakeFileStore;

namespace {

constexpr const char* DEST = "/bodies/x/.body.xhtml";

std::string contents(const FakeFileStore& store, const std::string& path) {
  auto it = store.files().find(path);
  if (it == store.files().end()) {
    return {};
  }
  return std::string(it->second.begin(), it->second.end());
}

size_t countOf(const std::string& haystack, const std::string& needle) {
  size_t n = 0;
  for (size_t p = haystack.find(needle); p != std::string::npos; p = haystack.find(needle, p + 1)) {
    ++n;
  }
  return n;
}

}  // namespace

// Baseline: the ordinary path emits exactly one prologue.
TEST(ArticleBodyWriter, SingleFetchEmitsOnePrologue) {
  FakeFileStore store;
  ArticleBodyWriter writer(store, DEST, "https://example.com/a", "T");

  ASSERT_TRUE(writer.onBodyChunk("<h2>A</h2><p>", 13));
  ASSERT_TRUE(writer.onBodyChunk("body text</p>", 13));
  ASSERT_TRUE(writer.onBodyEnd(true));

  const std::string out = contents(store, DEST);
  EXPECT_EQ(countOf(out, "<?xml"), 1u) << out;
  EXPECT_EQ(countOf(out, "<body>"), 1u);
}

// The regression this file exists for. The writer buffers output, so nothing
// reached the store -- and the old open_ guard stayed false -- until the buffer
// first filled. Every body arriving in more than one chunk therefore had a
// second prologue injected at the first chunk boundary, which is exactly what
// a device-captured article.epub contained.
TEST(ArticleBodyWriter, ManySmallChunksStillEmitOneWellFormedDocument) {
  FakeFileStore store;
  ArticleBodyWriter writer(store, DEST, "https://example.com/a", "T");

  static constexpr const char* kChunks[] = {"<h2>Heading</h2>", "<p>first para</p>", "<p>second para</p>",
                                            "<p>third para</p>"};
  for (const char* chunk : kChunks) {
    ASSERT_TRUE(writer.onBodyChunk(chunk, strlen(chunk)));
  }
  ASSERT_TRUE(writer.onBodyEnd(true));

  const std::string out = contents(store, DEST);
  EXPECT_EQ(countOf(out, "<?xml"), 1u) << out;
  EXPECT_EQ(countOf(out, "<html"), 1u) << out;
  EXPECT_EQ(countOf(out, "<body>"), 1u);
  EXPECT_EQ(countOf(out, "</body>"), 1u);
  EXPECT_EQ(countOf(out, "</html>"), 1u);
  // The prologue must lead the document, not appear part-way through it.
  EXPECT_EQ(out.rfind("<?xml", 0), 0u);
  EXPECT_NE(out.find("third para"), std::string::npos) << "body content lost";
}

// A retried download builds a fresh writer against the same destination.
TEST(ArticleBodyWriter, RetryAfterAbortDoesNotAppendToLeftovers) {
  FakeFileStore store;
  {
    ArticleBodyWriter first(store, DEST, "https://example.com/a", "T");
    ASSERT_TRUE(first.onBodyChunk("<h2>A</h2><p>", 13));
    // Torn transfer: destructor aborts the open write.
  }
  ArticleBodyWriter second(store, DEST, "https://example.com/a", "T");
  ASSERT_TRUE(second.onBodyChunk("<h2>A</h2><p>full</p>", 21));
  ASSERT_TRUE(second.onBodyEnd(true));

  const std::string out = contents(store, DEST);
  EXPECT_EQ(countOf(out, "<?xml"), 1u) << out;
}
