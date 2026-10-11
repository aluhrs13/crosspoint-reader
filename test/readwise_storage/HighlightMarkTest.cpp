// Quotes are stored as space-separated selectable tokens, with no positions.
// markHighlightWords is the whole matching rule the article page draws from:
// every full occurrence, and an edge fragment only when the quote does not
// fit on the page.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "lib/Readwise/ReadwiseHighlight.h"

namespace {

using namespace readwise;

void words(HighlightWordView* out, const char* const* text, uint16_t count) {
  for (uint16_t i = 0; i < count; ++i) {
    out[i].text = text[i];
    out[i].length = static_cast<uint16_t>(strlen(text[i]));
  }
}

uint8_t markAt(const char* const* page, uint16_t pageCount, const char* quote, uint8_t flags, uint8_t* marks) {
  HighlightWordView views[16];
  words(views, page, pageCount);
  const char* quotes[] = {quote};
  const uint8_t flagRow[] = {flags};
  markHighlightWords(views, pageCount, quotes, flagRow, 1, marks);
  return marks[0];
}

}  // namespace

TEST(HighlightMarkTest, FullOccurrenceIsPendingUntilPosted) {
  const char* page[] = {"alpha", "beta", "gamma"};
  uint8_t marks[3] = {9, 9, 9};
  markAt(page, 3, "alpha beta", HIGHLIGHT_FLAG_FAILED, marks);
  EXPECT_EQ(marks[0], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[1], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[2], static_cast<uint8_t>(HighlightMark::None));

  markAt(page, 3, "alpha beta", HIGHLIGHT_FLAG_POSTED, marks);
  EXPECT_EQ(marks[0], static_cast<uint8_t>(HighlightMark::Posted));
  EXPECT_EQ(marks[1], static_cast<uint8_t>(HighlightMark::Posted));
}

TEST(HighlightMarkTest, MarksEveryNonOverlappingFullOccurrence) {
  const char* page[] = {"red", "red", "blue", "red"};
  uint8_t marks[4] = {};
  HighlightWordView views[4];
  words(views, page, 4);
  const char* quotes[] = {"red"};
  const uint8_t flags[] = {HIGHLIGHT_FLAG_POSTED};
  markHighlightWords(views, 4, quotes, flags, 1, marks);
  EXPECT_EQ(marks[0], static_cast<uint8_t>(HighlightMark::Posted));
  EXPECT_EQ(marks[1], static_cast<uint8_t>(HighlightMark::Posted));
  EXPECT_EQ(marks[2], static_cast<uint8_t>(HighlightMark::None));
  EXPECT_EQ(marks[3], static_cast<uint8_t>(HighlightMark::Posted));
}

TEST(HighlightMarkTest, PostedWinsWhenTheSameWordsAreAlsoPending) {
  const char* page[] = {"one", "two", "three"};
  uint8_t marks[3] = {};
  HighlightWordView views[3];
  words(views, page, 3);
  const char* quotes[] = {"one two", "one two three"};
  const uint8_t flags[] = {0, HIGHLIGHT_FLAG_POSTED};
  markHighlightWords(views, 3, quotes, flags, 2, marks);
  EXPECT_EQ(marks[0], static_cast<uint8_t>(HighlightMark::Posted));
  EXPECT_EQ(marks[1], static_cast<uint8_t>(HighlightMark::Posted));
  EXPECT_EQ(marks[2], static_cast<uint8_t>(HighlightMark::Posted));
}

TEST(HighlightMarkTest, PartialMatchOnlyAtThePageEdges) {
  const char* page[] = {"the", "quick", "brown", "fox", "jumps"};
  uint8_t marks[5] = {};
  HighlightWordView views[5];
  words(views, page, 5);

  const char* prefix = "brown fox jumps over the lazy";
  const uint8_t pending[] = {0};
  markHighlightWords(views, 5, &prefix, pending, 1, marks);
  EXPECT_EQ(marks[0], static_cast<uint8_t>(HighlightMark::None));
  EXPECT_EQ(marks[1], static_cast<uint8_t>(HighlightMark::None));
  EXPECT_EQ(marks[2], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[3], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[4], static_cast<uint8_t>(HighlightMark::Pending));

  const char* suffix = "over the lazy the quick brown";
  markHighlightWords(views, 5, &suffix, pending, 1, marks);
  EXPECT_EQ(marks[0], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[1], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[2], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[3], static_cast<uint8_t>(HighlightMark::None));
  EXPECT_EQ(marks[4], static_cast<uint8_t>(HighlightMark::None));

  const char* middle = "quick brown fox extra words here";
  markHighlightWords(views, 5, &middle, pending, 1, marks);
  for (uint8_t mark : marks) {
    EXPECT_EQ(mark, static_cast<uint8_t>(HighlightMark::None));
  }
}

TEST(HighlightMarkTest, ShortEdgeFragmentIsNotMarked) {
  const char* page[] = {"only", "two", "left"};
  uint8_t marks[3] = {9, 9, 9};
  HighlightWordView views[3];
  words(views, page, 3);
  const char* quote = "two left behind";
  const uint8_t pending[] = {0};
  markHighlightWords(views, 3, &quote, pending, 1, marks);
  EXPECT_EQ(marks[0], static_cast<uint8_t>(HighlightMark::None));
  EXPECT_EQ(marks[1], static_cast<uint8_t>(HighlightMark::None));
  EXPECT_EQ(marks[2], static_cast<uint8_t>(HighlightMark::None));
}

TEST(HighlightMarkTest, FullMatchSuppressesAShorterEdgeFragment) {
  const char* page[] = {"alpha", "beta", "gamma", "alpha"};
  uint8_t marks[4] = {};
  HighlightWordView views[4];
  words(views, page, 4);
  const char* quote = "alpha beta gamma";
  const uint8_t pending[] = {0};
  markHighlightWords(views, 4, &quote, pending, 1, marks);
  EXPECT_EQ(marks[0], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[1], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[2], static_cast<uint8_t>(HighlightMark::Pending));
  EXPECT_EQ(marks[3], static_cast<uint8_t>(HighlightMark::None));
}
