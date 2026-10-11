#pragma once

#include <cctype>
#include <cstdint>

// A word the highlight gesture can select. Punctuation-only tokens are skipped
// so a stored quote is the same sequence the page marker searches for.

inline bool isSelectableToken(const char* text) {
  if (text == nullptr) {
    return false;
  }
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) {
        return true;
      }
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) {
        break;
      }
      p += 2;
    } else {
      return true;
    }
  }
  return false;
}
