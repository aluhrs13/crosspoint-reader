add_executable(SleepCalendarTest SleepCalendarTest.cpp)

target_include_directories(SleepCalendarTest PRIVATE ../../src)

target_link_libraries(SleepCalendarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepCalendarTest)
