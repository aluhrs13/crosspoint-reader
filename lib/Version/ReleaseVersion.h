#pragma once

#include <string_view>

namespace ReleaseVersion {

bool isNewer(std::string_view latest, std::string_view current);

}  // namespace ReleaseVersion
