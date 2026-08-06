add_executable(ReadwiseHighlightsTest
  ReadwiseHighlightsTest.cpp
  ReadwiseHighlightStoreTest.cpp
  ReadwiseHighlightSyncTest.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseDocument.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseCodec.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseJournal.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseSyncEngine.cpp
  ${REPO_ROOT}/lib/Readwise/BodyTextWriter.cpp
  ${REPO_ROOT}/lib/Readwise/HtmlTextExtractor.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseHighlights.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseHighlightStore.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseHighlightSync.cpp
)

target_include_directories(ReadwiseHighlightsTest PRIVATE
  ${REPO_ROOT}/lib/Readwise
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/readwise_storage
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ReadwiseHighlightsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadwiseHighlightsTest)
