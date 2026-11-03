#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A JPEG thumbnail of an image file, its long side at most `max_dimension` (never upscaled), EXIF orientation applied.
 * Apple decodes it downsampled (ImageIO); other platforms load the picture (refused above 64 million pixels) and
 * scale it. Fails, never throws.
 */
Roe<std::vector<uint8_t>> MakeImageThumbnailJpeg(const std::string& image_path, int max_dimension);

} // namespace pbr
