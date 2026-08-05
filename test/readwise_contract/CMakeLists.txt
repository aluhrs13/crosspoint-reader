add_executable(ReadwiseContractTest
  ReadwiseContractTest.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(ReadwiseContractTest PRIVATE
  ${REPO_ROOT}/lib/JsonParser
)

target_compile_definitions(ReadwiseContractTest PRIVATE
  READWISE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(ReadwiseContractTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadwiseContractTest)
