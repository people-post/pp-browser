#pragma once

#include "common/Error.h"

#include <cstdint>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A JPEG thumbnail of an image held in memory (the plaintext is never written out for it), its long side at most
 * `max_dimension` (never upscaled), EXIF orientation applied. Apple decodes it downsampled (ImageIO); other platforms
 * load the picture (refused above 64 million pixels) and scale it. Fails, never throws.
 */
Roe<std::vector<uint8_t>> MakeImageThumbnailJpeg(const std::vector<uint8_t>& image_bytes, int max_dimension);

} // namespace pbr
