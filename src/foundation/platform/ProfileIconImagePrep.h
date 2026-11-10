#pragma once

#include "common/Error.h"

#include <SDL3/SDL_rect.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct EncodedProfileIcon {
  std::vector<uint8_t> bytes;
  std::string content_type;
  std::string kind;
  std::string file_extension;
};

/**
 * Load, straighten, crop, scale, and encode an image file for relay profile icon upload.
 * The file is read through SDL's IO (Android's picker hands back content:// addresses only SDL can open) and
 * turned upright by its EXIF orientation. `crop` is the part to keep, in pixels of the upright picture (a rectangle
 * reaching outside is clamped to the picture); without it the largest centred square is kept, so the icon is square
 * either way. Failures the person can act on carry their own words (Error::user): the picture cannot be opened, or
 * it is still too large after scaling.
 */
Roe<EncodedProfileIcon> PrepareProfileIconFromFile(const std::string& path, size_t max_bytes = 512u * 1024u,
                                                   int max_dimension = 256,
                                                   std::optional<SDL_Rect> crop = std::nullopt);

} // namespace pbr
