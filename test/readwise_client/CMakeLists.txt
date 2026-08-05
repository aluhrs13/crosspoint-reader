add_executable(ReadwiseClientTest
  ReadwiseClientTest.cpp
  HtmlTextExtractorTest.cpp
  ReadwiseListParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseDocument.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseCodec.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseListParser.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseClientCore.cpp
  ${REPO_ROOT}/lib/Readwise/HtmlTextExtractor.cpp
  ${REPO_ROOT}/lib/Readwise/BodyTextWriter.cpp
)

target_include_directories(ReadwiseClientTest PRIVATE
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Readwise
  ${REPO_ROOT}/test/readwise_storage
)

target_compile_definitions(ReadwiseClientTest PRIVATE
  READWISE_FIXTURES_DIR="${REPO_ROOT}/test/readwise_contract/fixtures"
)

target_link_libraries(ReadwiseClientTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadwiseClientTest)
