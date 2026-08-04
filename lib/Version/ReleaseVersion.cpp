#include "ReleaseVersion.h"

#include <cctype>
#include <cstdint>
#include <limits>

namespace {

struct Version {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;
  std::string_view prerelease;
};

bool parseNumber(std::string_view value, size_t& position, uint32_t& number) {
  if (position >= value.size() || !std::isdigit(static_cast<unsigned char>(value[position]))) {
    return false;
  }

  number = 0;
  do {
    const uint32_t digit = static_cast<uint32_t>(value[position] - '0');
    if (number > (std::numeric_limits<uint32_t>::max() - digit) / 10) {
      return false;
    }
    number = number * 10 + digit;
    ++position;
  } while (position < value.size() && std::isdigit(static_cast<unsigned char>(value[position])));

  return true;
}

bool isValidIdentifierList(std::string_view value, bool enforceNumericLeadingZero) {
  if (value.empty()) {
    return false;
  }

  size_t start = 0;
  while (start < value.size()) {
    const size_t end = value.find('.', start);
    const size_t length = (end == std::string_view::npos ? value.size() : end) - start;
    if (length == 0) {
      return false;
    }

    bool numeric = true;
    for (size_t i = start; i < start + length; ++i) {
      const unsigned char ch = static_cast<unsigned char>(value[i]);
      if (!std::isalnum(ch) && ch != '-') {
        return false;
      }
      numeric = numeric && std::isdigit(ch);
    }
    if (enforceNumericLeadingZero && numeric && length > 1 && value[start] == '0') {
      return false;
    }

    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  return true;
}

bool parse(std::string_view value, Version& version) {
  size_t position = 0;
  if (!value.empty() && value.front() == 'v') {
    ++position;
  }

  if (!parseNumber(value, position, version.major) || position >= value.size() || value[position++] != '.' ||
      !parseNumber(value, position, version.minor) || position >= value.size() || value[position++] != '.' ||
      !parseNumber(value, position, version.patch)) {
    return false;
  }

  if (position < value.size() && value[position] == '-') {
    const size_t prereleaseStart = ++position;
    const size_t buildStart = value.find('+', position);
    const size_t prereleaseEnd = buildStart == std::string_view::npos ? value.size() : buildStart;
    version.prerelease = value.substr(prereleaseStart, prereleaseEnd - prereleaseStart);
    if (!isValidIdentifierList(version.prerelease, true)) {
      return false;
    }
    position = prereleaseEnd;
  }

  if (position < value.size() && value[position] == '+') {
    const std::string_view build = value.substr(position + 1);
    if (!isValidIdentifierList(build, false)) {
      return false;
    }
    position = value.size();
  }

  return position == value.size();
}

int compareIdentifier(std::string_view left, std::string_view right) {
  const bool leftNumeric = left.find_first_not_of("0123456789") == std::string_view::npos;
  const bool rightNumeric = right.find_first_not_of("0123456789") == std::string_view::npos;

  if (leftNumeric && rightNumeric) {
    if (left.size() != right.size()) {
      return left.size() < right.size() ? -1 : 1;
    }
  } else if (leftNumeric != rightNumeric) {
    return leftNumeric ? -1 : 1;
  }

  const int comparison = left.compare(right);
  return comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
}

int comparePrerelease(std::string_view left, std::string_view right) {
  if (left.empty() || right.empty()) {
    if (left.empty() == right.empty()) {
      return 0;
    }
    return left.empty() ? 1 : -1;
  }

  size_t leftStart = 0;
  size_t rightStart = 0;
  while (leftStart < left.size() && rightStart < right.size()) {
    const size_t leftEnd = left.find('.', leftStart);
    const size_t rightEnd = right.find('.', rightStart);
    const auto leftIdentifier = left.substr(leftStart, leftEnd - leftStart);
    const auto rightIdentifier = right.substr(rightStart, rightEnd - rightStart);
    const int comparison = compareIdentifier(leftIdentifier, rightIdentifier);
    if (comparison != 0) {
      return comparison;
    }

    leftStart = leftEnd == std::string_view::npos ? left.size() : leftEnd + 1;
    rightStart = rightEnd == std::string_view::npos ? right.size() : rightEnd + 1;
  }

  if (leftStart == left.size() && rightStart == right.size()) {
    return 0;
  }
  return leftStart == left.size() ? -1 : 1;
}

}  // namespace

bool ReleaseVersion::isNewer(const std::string_view latest, const std::string_view current) {
  Version latestVersion;
  Version currentVersion;
  if (!parse(latest, latestVersion) || !parse(current, currentVersion)) {
    return false;
  }

  if (latestVersion.major != currentVersion.major) {
    return latestVersion.major > currentVersion.major;
  }
  if (latestVersion.minor != currentVersion.minor) {
    return latestVersion.minor > currentVersion.minor;
  }
  if (latestVersion.patch != currentVersion.patch) {
    return latestVersion.patch > currentVersion.patch;
  }

  return comparePrerelease(latestVersion.prerelease, currentVersion.prerelease) > 0;
}
