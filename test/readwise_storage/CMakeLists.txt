add_executable(ReadwiseStorageTest
  ReadwiseCodecTest.cpp
  ReadwiseSyncEngineTest.cpp
  ZipWriterTest.cpp
  ZipWriterExternalTest.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseDocument.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseCodec.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseJournal.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseSyncEngine.cpp
  ${REPO_ROOT}/lib/Readwise/BodyTextWriter.cpp
  ${REPO_ROOT}/lib/Readwise/HtmlTextExtractor.cpp
  ${REPO_ROOT}/lib/Readwise/ZipWriter.cpp
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
