#pragma once

// This fork publishes its own releases. OTA looks there, and still selects
// crosspoint-<version>-<device>.bin the way upstream does. The Readwise
// variant sets OTA_ASSET_DEVICE so it installs its own asset instead of the
// board image.

namespace ForkConfig {

inline constexpr char OTA_LATEST_RELEASE_URL[] =
    "https://api.github.com/repos/aluhrs13/crosspoint-reader/releases/latest";

}  // namespace ForkConfig
