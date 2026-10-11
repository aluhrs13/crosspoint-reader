#pragma once

#include <cstdint>

// Sunday is 0, matching struct tm::tm_wday, so the sleep calendar does not
// have to translate the clock's weekday into another origin.
inline constexpr int kCalendarWeekdays = 7;

inline bool isLeapYear(const int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

inline int daysInMonth(const int year, const int month) {
  static constexpr uint8_t kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return kDays[month];
}

// Weekday of the 1st, given any day in that month and that day's weekday.
inline int weekdayOfFirst(const int dayOfMonth, const int weekday) {
  const int back = (dayOfMonth - 1) % kCalendarWeekdays;
  int first = weekday - back;
  if (first < 0) first += kCalendarWeekdays;
  return first;
}

inline int weeksInMonth(const int firstWeekday, const int days) {
  return (firstWeekday + days + kCalendarWeekdays - 1) / kCalendarWeekdays;
}
