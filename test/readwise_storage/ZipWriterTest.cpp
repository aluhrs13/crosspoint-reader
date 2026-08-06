// Structural tests for the store-only ZIP writer.
//
// The device cannot tell us whether the archives it produces are valid -- a
// malformed central directory would surface only as "the article renders as
// nothing", long after the fact. So these assert the byte layout directly
// against the spec, and ZipWriterExternalTest additionally hands the output to
// a real ZIP implementation.

#include "lib/Readwise/ZipWriter.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "FakeReadwise.h"

using readwise::ZipWriter;
using readwise::zipCrc32;
using testing_support::FakeFileStore;

namespace {

uint16_t read16(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

uint32_t read32(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) |
         (static_cast<uint32_t>(data[offset + 3]) << 24);
}

std::string nameAt(const std::vector<uint8_t>& data, size_t offset, size_t len) {
  return std::string(reinterpret_cast<const char*>(data.data()) + offset, len);
}

}  // namespace

TEST(ZipCrc32, MatchesKnownVectors) {
  // The canonical check value from the CRC-32/ISO-HDLC definition.
  const std::string check = "123456789";
  EXPECT_EQ(zipCrc32(0, reinterpret_cast<const uint8_t*>(check.data()), check.size()), 0xCBF43926u);
  EXPECT_EQ(zipCrc32(0, nullptr, 0), 0u);

  // Incremental updates must equal a single pass over the concatenation.
  const std::string a = "12345";
  const std::string b = "6789";
  uint32_t crc = zipCrc32(0, reinterpret_cast<const uint8_t*>(a.data()), a.size());
  crc = zipCrc32(crc, reinterpret_cast<const uint8_t*>(b.data()), b.size());
  EXPECT_EQ(crc, 0xCBF43926u);
}

TEST(ZipWriter, WritesWellFormedSingleEntryArchive) {
  FakeFileStore store;
  ZipWriter zip(store);

  ASSERT_TRUE(zip.begin("/a.epub"));
  ASSERT_TRUE(zip.addEntry("mimetype", std::string("application/epub+zip")));
  ASSERT_TRUE(zip.finish());

  ASSERT_TRUE(store.has("/a.epub"));
  const std::vector<uint8_t>& archive = store.files().at("/a.epub");

  const std::string payload = "application/epub+zip";
  const size_t nameLen = 8;  // "mimetype"
  const size_t localSize = 30 + nameLen + payload.size();

  // Local file header.
  EXPECT_EQ(read32(archive, 0), 0x04034b50u);
  EXPECT_EQ(read16(archive, 8), 0u) << "method must be stored";
  EXPECT_EQ(read32(archive, 14),
            zipCrc32(0, reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
  EXPECT_EQ(read32(archive, 18), payload.size()) << "compressed size";
  EXPECT_EQ(read32(archive, 22), payload.size()) << "uncompressed size";
  EXPECT_EQ(read16(archive, 26), nameLen);
  EXPECT_EQ(read16(archive, 28), 0u) << "no extra field";
  EXPECT_EQ(nameAt(archive, 30, nameLen), "mimetype");
  EXPECT_EQ(nameAt(archive, 30 + nameLen, payload.size()), payload);

  // Central directory.
  EXPECT_EQ(read32(archive, localSize), 0x02014b50u);
  EXPECT_EQ(read16(archive, localSize + 10), 0u) << "method must be stored";
  EXPECT_EQ(read32(archive, localSize + 42), 0u) << "local header offset";
  EXPECT_EQ(nameAt(archive, localSize + 46, nameLen), "mimetype");

  // End of central directory.
  const size_t eocd = archive.size() - 22;
  EXPECT_EQ(read32(archive, eocd), 0x06054b50u);
  EXPECT_EQ(read16(archive, eocd + 8), 1u);
  EXPECT_EQ(read16(archive, eocd + 10), 1u);
  EXPECT_EQ(read32(archive, eocd + 12), 46u + nameLen) << "central directory size";
  EXPECT_EQ(read32(archive, eocd + 16), localSize) << "central directory offset";
  EXPECT_EQ(read16(archive, eocd + 20), 0u) << "no archive comment";
}

TEST(ZipWriter, RecordsCorrectLocalOffsetsForMultipleEntries) {
  FakeFileStore store;
  ZipWriter zip(store);

  ASSERT_TRUE(zip.begin("/a.epub"));
  ASSERT_TRUE(zip.addEntry("a.txt", std::string("first")));
  ASSERT_TRUE(zip.addEntry("bb.txt", std::string("second payload")));
  ASSERT_TRUE(zip.addEntry("ccc.txt", std::string()));
  ASSERT_EQ(zip.entryCount(), 3u);
  ASSERT_TRUE(zip.finish());

  const std::vector<uint8_t>& archive = store.files().at("/a.epub");
  const size_t firstSize = 30 + 5 + 5;
  const size_t secondSize = 30 + 6 + 14;

  const size_t centralDirOffset = read32(archive, archive.size() - 22 + 16);
  EXPECT_EQ(read32(archive, centralDirOffset + 42), 0u);
  EXPECT_EQ(read32(archive, centralDirOffset + 46 + 5 + 42), firstSize);
  EXPECT_EQ(read32(archive, centralDirOffset + (46 + 5) + (46 + 6) + 42), firstSize + secondSize);

  // A zero-length entry is legal and must still carry a crc of 0.
  const size_t thirdCentral = centralDirOffset + (46 + 5) + (46 + 6);
  EXPECT_EQ(read32(archive, thirdCentral + 16), 0u);
  EXPECT_EQ(read32(archive, thirdCentral + 20), 0u);
}

TEST(ZipWriter, StreamedEntryMatchesSingleShotEntry) {
  FakeFileStore streamed;
  FakeFileStore whole;

  ZipWriter a(streamed);
  ASSERT_TRUE(a.begin("/x.epub"));
  ASSERT_TRUE(a.beginEntry("f.bin"));
  const std::string part1 = "hello ";
  const std::string part2 = "world";
  ASSERT_TRUE(a.writeData(reinterpret_cast<const uint8_t*>(part1.data()), part1.size()));
  ASSERT_TRUE(a.writeData(reinterpret_cast<const uint8_t*>(part2.data()), part2.size()));
  ASSERT_TRUE(a.endEntry());
  ASSERT_TRUE(a.finish());

  ZipWriter b(whole);
  ASSERT_TRUE(b.begin("/x.epub"));
  ASSERT_TRUE(b.addEntry("f.bin", std::string("hello world")));
  ASSERT_TRUE(b.finish());

  EXPECT_EQ(streamed.files().at("/x.epub"), whole.files().at("/x.epub"));
}

TEST(ZipWriter, CopiesAnExistingStoreFileIntoTheArchive) {
  FakeFileStore store;
  // Larger than the 1 KB copy chunk, so the multi-read loop is exercised.
  std::vector<uint8_t> payload(2600);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i * 7);
  }
  store.put("/img.jpg", payload);

  ZipWriter zip(store);
  ASSERT_TRUE(zip.begin("/a.epub"));
  ASSERT_TRUE(zip.addEntryFromFile("OEBPS/images/0.jpg", "/img.jpg"));
  ASSERT_TRUE(zip.finish());

  const std::vector<uint8_t>& archive = store.files().at("/a.epub");
  const size_t nameLen = 18;
  EXPECT_EQ(read32(archive, 18), payload.size());
  EXPECT_EQ(read32(archive, 14), zipCrc32(0, payload.data(), payload.size()));
  const std::vector<uint8_t> stored(archive.begin() + 30 + nameLen,
                                    archive.begin() + 30 + nameLen + payload.size());
  EXPECT_EQ(stored, payload);
}

// A missing source is rejected before any header is written, so the archive
// stays consistent and the caller can carry on. This is what lets a failed
// image download degrade to alt text instead of losing the whole article.
TEST(ZipWriter, MissingSourceFileIsSkippableWithoutPoisoningTheArchive) {
  FakeFileStore store;
  ZipWriter zip(store);

  ASSERT_TRUE(zip.begin("/a.epub"));
  ASSERT_TRUE(zip.addEntry("ok.txt", std::string("fine")));
  EXPECT_FALSE(zip.addEntryFromFile("missing.jpg", "/nope.jpg"));
  EXPECT_EQ(zip.entryCount(), 1u) << "the failed entry must leave no trace";
  ASSERT_TRUE(zip.addEntry("after.txt", std::string("still works")));
  ASSERT_TRUE(zip.finish());

  ASSERT_TRUE(store.has("/a.epub"));
  const std::vector<uint8_t>& archive = store.files().at("/a.epub");
  EXPECT_EQ(read16(archive, archive.size() - 22 + 8), 2u);
}

// The opposite case: the source goes unreadable after the local header and
// some payload are already down. The entry cannot be un-written, so the whole
// archive has to be abandoned rather than committed truncated.
TEST(ZipWriter, SourceFailingMidCopyPoisonsTheArchive) {
  FakeFileStore store;
  store.put("/img.jpg", std::vector<uint8_t>(2600, 0xAB));
  store.failAtRead(2);

  ZipWriter zip(store);
  ASSERT_TRUE(zip.begin("/a.epub"));
  EXPECT_FALSE(zip.addEntryFromFile("OEBPS/images/0.jpg", "/img.jpg"));
  EXPECT_FALSE(zip.finish());
  EXPECT_FALSE(store.has("/a.epub"));
}

TEST(ZipWriter, AbortWritesNothing) {
  FakeFileStore store;
  {
    ZipWriter zip(store);
    ASSERT_TRUE(zip.begin("/a.epub"));
    ASSERT_TRUE(zip.addEntry("a.txt", std::string("payload")));
    zip.abort();
  }
  EXPECT_FALSE(store.has("/a.epub"));
}

TEST(ZipWriter, DestructorAbortsAnUnfinishedArchive) {
  FakeFileStore store;
  {
    ZipWriter zip(store);
    ASSERT_TRUE(zip.begin("/a.epub"));
    ASSERT_TRUE(zip.addEntry("a.txt", std::string("payload")));
    // Falls out of scope without finish(), as it would on an early return.
  }
  EXPECT_FALSE(store.has("/a.epub"));
  // The store's incremental write must be released, or the next one fails.
  ZipWriter next(store);
  EXPECT_TRUE(next.begin("/b.epub"));
}

TEST(ZipWriter, RejectsNamesAndEntryCountsBeyondTheCaps) {
  FakeFileStore store;
  ZipWriter zip(store);
  ASSERT_TRUE(zip.begin("/a.epub"));

  EXPECT_FALSE(zip.beginEntry(""));
  EXPECT_FALSE(zip.beginEntry(std::string(readwise::ZIP_NAME_CAP, 'x')));

  for (size_t i = 0; i < readwise::ZIP_MAX_ENTRIES; ++i) {
    ASSERT_TRUE(zip.addEntry("e" + std::to_string(i), std::string("x")));
  }
  EXPECT_FALSE(zip.beginEntry("one-too-many"));
  EXPECT_TRUE(zip.finish());
}

TEST(ZipWriter, RejectsNestedEntriesAndFinishWithAnOpenEntry) {
  FakeFileStore store;
  ZipWriter zip(store);
  ASSERT_TRUE(zip.begin("/a.epub"));
  ASSERT_TRUE(zip.beginEntry("a.txt"));
  EXPECT_FALSE(zip.beginEntry("b.txt"));
  EXPECT_FALSE(zip.finish()) << "finishing mid-entry would truncate the payload";
  EXPECT_FALSE(store.has("/a.epub"));
}

TEST(ZipWriter, FailedCommitLeavesNoFile) {
  FakeFileStore store;
  store.failAtWrite(1);  // the commit is the first durable write here

  ZipWriter zip(store);
  ASSERT_TRUE(zip.begin("/a.epub"));
  ASSERT_TRUE(zip.addEntry("a.txt", std::string("payload")));
  EXPECT_FALSE(zip.finish());
  EXPECT_FALSE(store.has("/a.epub"));
}
