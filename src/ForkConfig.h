#pragma once

// Which release asset this build installs over OTA. A release publishes one
// asset per firmware variant, so each variant must ask for its own or it would
// flash a different variant's image. Override per environment in
// platformio.ini; builds that don't set it get the standard firmware.
#ifndef OTA_FIRMWARE_ASSET_NAME
#define OTA_FIRMWARE_ASSET_NAME "firmware.bin"
#endif

namespace ForkConfig {

inline constexpr char OTA_LATEST_RELEASE_URL[] =
    "https://api.github.com/repos/aluhrs13/crosspoint-reader/releases/latest";

inline constexpr char OTA_FIRMWARE_ASSET[] = OTA_FIRMWARE_ASSET_NAME;

}  // namespace ForkConfig
