#pragma once

#include "mesh/types/MediaChannel.h"

#include <cstdint>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * What this client sends at a video level (docs/contracts/MEDIA_CHANNELS.md: levels are ordered,
 * their meaning is the client's). Level 1 is the calls' single level.
 */
struct VideoLevelProfile {
  int width = 640;
  int height = 360;
  int64_t target_bps = 250'000;
};

inline VideoLevelProfile ProfileForVideoLevel(const uint8_t level) {
  if (level <= mesh::kDefaultVideoLevel) {
    return {640, 360, 250'000}; // a call's: fits a ~600 kbit/s phone uplink with audio (lab video_*_narrow)
  }
  if (level == 2) {
    return {1280, 720, 1'200'000};
  }
  return {1920, 1080, 2'500'000};
}

} // namespace pbr
