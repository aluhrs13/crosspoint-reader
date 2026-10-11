#include <gtest/gtest.h>

#include "activities/boot_sleep/SleepCalendar.h"

TEST(SleepCalendar, October2026StartsThursday) {
  // 5 October 2026 is a Monday. The 1st is the Thursday before it.
  EXPECT_EQ(daysInMonth(2026, 10), 31);
  EXPECT_EQ(weekdayOfFirst(5, 1), 4);
  EXPECT_EQ(weeksInMonth(4, 31), 5);
}

TEST(SleepCalendar, LeapDay) {
  EXPECT_TRUE(isLeapYear(2024));
  EXPECT_TRUE(isLeapYear(2000));
  EXPECT_FALSE(isLeapYear(1900));
  EXPECT_FALSE(isLeapYear(2026));
  EXPECT_EQ(daysInMonth(2024, 2), 29);
  EXPECT_EQ(daysInMonth(2026, 2), 28);
  EXPECT_EQ(daysInMonth(2026, 0), 0);
  EXPECT_EQ(daysInMonth(2026, 13), 0);
}

TEST(SleepCalendar, WeekCountWrapsTheGrid) {
  // A 31-day month that starts on Saturday needs six rows.
  EXPECT_EQ(weeksInMonth(6, 31), 6);
  // A 28-day February that starts on Sunday is exactly four.
  EXPECT_EQ(weeksInMonth(0, 28), 4);
}
