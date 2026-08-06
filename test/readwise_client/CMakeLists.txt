# The article XHTML must satisfy the same parser the firmware uses, so the
# tests link the vendored expat with the firmware's own XML_GE=0 build flags
# (platformio.ini:32-34) rather than a system copy. Asserting on expected
# output strings would only re-state our assumptions; expat does not share
# them.
add_library(crosspoint_expat STATIC
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)
target_include_directories(crosspoint_expat PUBLIC ${REPO_ROOT}/lib/expat)
target_compile_definitions(crosspoint_expat PUBLIC
  XML_GE=0
  XML_CONTEXT_BYTES=1024
  XML_STATIC
  XML_POOR_ENTROPY
)

add_executable(ReadwiseClientTest
  ReadwiseClientTest.cpp
  HtmlTextExtractorTest.cpp
  HtmlTokenizerTest.cpp
  ArticleXhtmlWriterTest.cpp
  ArticleImageUrlTest.cpp
  ReadwiseListParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseDocument.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseCodec.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseListParser.cpp
  ${REPO_ROOT}/lib/Readwise/ReadwiseClientCore.cpp
  ${REPO_ROOT}/lib/Readwise/HtmlTextExtractor.cpp
  ${REPO_ROOT}/lib/Readwise/HtmlTokenizer.cpp
  ${REPO_ROOT}/lib/Readwise/ArticleImageUrl.cpp
  ${REPO_ROOT}/lib/Readwise/ArticleXhtmlWriter.cpp
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
  crosspoint_expat
  GTest::gtest_main
)

gtest_discover_tests(ReadwiseClientTest)
