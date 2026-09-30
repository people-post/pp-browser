#pragma once

#include "common/media/MediaChannel.h"

#include <cstdint>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What this client sends at a video level (docs/contracts/MEDIA_CHANNELS.md: levels are ordered,
 * their meaning is the client's). Level 1 is the calls' single level.
 */
struct VideoLevelProfile {
  int width = 640;
  int height = 360;
  int64_t target_bps = 400'000;
};

inline VideoLevelProfile ProfileForVideoLevel(const uint8_t level) {
  if (level <= kDefaultVideoLevel) {
    return {640, 360, 400'000};
  }
  if (level == 2) {
    return {1280, 720, 1'200'000};
  }
  return {1920, 1080, 2'500'000};
}

} // namespace pbr
