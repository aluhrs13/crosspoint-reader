add_executable(ReleaseVersionTest
  ReleaseVersionTest.cpp
  ${REPO_ROOT}/lib/Version/ReleaseVersion.cpp
)

target_include_directories(ReleaseVersionTest PRIVATE
  ${REPO_ROOT}/lib/Version
)

target_link_libraries(ReleaseVersionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReleaseVersionTest)
