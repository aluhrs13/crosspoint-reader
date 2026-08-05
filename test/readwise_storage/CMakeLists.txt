add_executable(ReadwiseStorageTest
  ReadwiseCodecTest.cpp
  ReadwiseSyncEngineTest.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseDocument.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseCodec.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseJournal.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseSyncEngine.cpp
)

target_include_directories(ReadwiseStorageTest PRIVATE
  ${REPO_ROOT}/lib/Readwise
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ReadwiseStorageTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadwiseStorageTest)
