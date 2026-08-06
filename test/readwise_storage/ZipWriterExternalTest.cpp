// Cross-check ZipWriter's output against a real ZIP implementation.
//
// The structural assertions in ZipWriterTest.cpp encode our reading of the
// spec; if that reading is wrong they will agree with the bug. Handing the
// bytes to Python's zipfile -- which validates the central directory, the
// local headers, and every CRC -- is the check that does not share our
// assumptions. It is also the fastest way to notice a regression, since the
// on-device consumer (lib/ZipFile) cannot be built on the host.
//
// Skipped, not failed, when no Python interpreter is on PATH.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "FakeReadwise.h"
#include "lib/Readwise/ZipWriter.h"

using readwise::ZipWriter;
using testing_support::FakeFileStore;

namespace {

#ifndef ZIP_TEST_SCRATCH_DIR
#define ZIP_TEST_SCRATCH_DIR "."
#endif

// Quoting differs between cmd.exe and sh, but both accept a double-quoted
// argument containing no quotes of its own, and the scratch path is generated
// by CMake rather than user input.
std::string quoted(const std::string& value) { return "\"" + value + "\""; }

const char* findPython() {
  static const char* kCandidates[] = {"python3", "python", "py"};
  for (const char* candidate : kCandidates) {
    const std::string probe = std::string(candidate) + " -c \"import zipfile\" >" +
#ifdef _WIN32
                              "NUL 2>NUL";
#else
                              "/dev/null 2>/dev/null";
#endif
    if (std::system(probe.c_str()) == 0) {
      return candidate;
    }
  }
  return nullptr;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return out.good();
}

}  // namespace

TEST(ZipWriterExternal, PythonZipfileAcceptsTheArchive) {
  const char* python = findPython();
  if (python == nullptr) {
    GTEST_SKIP() << "no Python interpreter with zipfile on PATH";
  }

  FakeFileStore store;
  // A binary payload spanning the whole byte range, so a signed-char or
  // encoding slip anywhere in the write path shows up as a CRC mismatch.
  std::vector<uint8_t> binary(1024);
  for (size_t i = 0; i < binary.size(); ++i) {
    binary[i] = static_cast<uint8_t>(i);
  }
  store.put("/src.bin", binary);

  ZipWriter zip(store);
  ASSERT_TRUE(zip.begin("/article.epub"));
  ASSERT_TRUE(zip.addEntry("mimetype", std::string("application/epub+zip")));
  ASSERT_TRUE(zip.addEntry("META-INF/container.xml", std::string("<container/>")));
  ASSERT_TRUE(zip.addEntry("OEBPS/article.xhtml", std::string("<html><body>hi</body></html>")));
  ASSERT_TRUE(zip.addEntry("OEBPS/empty.txt", std::string()));
  ASSERT_TRUE(zip.addEntryFromFile("OEBPS/images/0.jpg", "/src.bin"));
  ASSERT_TRUE(zip.finish());

  const std::string path = std::string(ZIP_TEST_SCRATCH_DIR) + "/zipwriter_crosscheck.epub";
  ASSERT_TRUE(writeFile(path, store.files().at("/article.epub")));

  // testzip() returns the first corrupt member's name, or None. The script
  // additionally asserts the member list, the stored method, and two payloads.
  const std::string script =
      "import sys,zipfile;"
      "z=zipfile.ZipFile(sys.argv[1]);"
      "assert z.testzip() is None, z.testzip();"
      "assert z.namelist()==['mimetype','META-INF/container.xml','OEBPS/article.xhtml',"
      "'OEBPS/empty.txt','OEBPS/images/0.jpg'], z.namelist();"
      "assert all(i.compress_type==zipfile.ZIP_STORED for i in z.infolist());"
      "assert z.read('mimetype')==b'application/epub+zip';"
      "assert z.read('OEBPS/empty.txt')==b'';"
      "assert z.read('OEBPS/images/0.jpg')==bytes(range(256))*4";

  const std::string command =
      std::string(python) + " -c " + quoted(script) + " " + quoted(path);
  EXPECT_EQ(std::system(command.c_str()), 0) << "Python zipfile rejected the archive at " << path;

  std::remove(path.c_str());
}
