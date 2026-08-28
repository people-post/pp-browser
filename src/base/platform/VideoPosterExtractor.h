#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr int kDefaultVideoPosterMaxDimension = 480;

/** Dark 16:9 placeholder JPEG within max_dimension (play triangle). Used as fallback. */
Roe<std::vector<uint8_t>> SoftVideoPosterJpeg(int max_dimension = kDefaultVideoPosterMaxDimension);

/** Best-effort JPEG bytes of an early frame. Never throws; may return soft placeholder. */
Roe<std::vector<uint8_t>> ExtractVideoPosterJpeg(const std::string& video_path,
                                                 int max_dimension = kDefaultVideoPosterMaxDimension);

} // namespace pbr
