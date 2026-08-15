add_executable(ReadwiseStorageTest
  ReadwiseCodecTest.cpp
  ReadwiseSyncEngineTest.cpp
  ZipWriterTest.cpp
  ArticleAssemblerTest.cpp
  ArticleBodyWriterTest.cpp
  ZipWriterExternalTest.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseDocument.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseCodec.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseJournal.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseSyncEngine.cpp
  ${REPO_ROOT}/lib/Readwise/HtmlTokenizer.cpp
  ${REPO_ROOT}/lib/Readwise/ZipWriter.cpp
  ${REPO_ROOT}/lib/Readwise/ArticleImageUrl.cpp
  ${REPO_ROOT}/lib/Readwise/ArticleXhtmlWriter.cpp
  ${REPO_ROOT}/lib/Readwise/ArticleBodyWriter.cpp
  ${REPO_ROOT}/lib/Readwise/ArticleAssembler.cpp
)

target_include_directories(ReadwiseStorageTest PRIVATE
  ${REPO_ROOT}/lib/Readwise
  ${REPO_ROOT}/lib/Memory
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# ZipWriterExternalTest drops an archive here to hand to Python's zipfile.
target_compile_definitions(ReadwiseStorageTest PRIVATE
  ZIP_TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(ReadwiseStorageTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadwiseStorageTest)
