#pragma once

#include "common/Error.h"
#include "foundation/platform/ImageAffine.h"

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
 * What the avatar crop page chose: the square `side` x `side` picture whose pixels (u, v) are read from the upright
 * source at `affine` (u, v) (AvatarCropFrame::OutputAffine; SampleAffine does the reading).
 */
struct ProfileIconCrop {
  ImageAffine affine;
  int side = 0;
};

/** A picture file decoded upright for the crop page: its real size and a copy of at most `max_edge` px on the long side. */
struct ProfilePhotoPreview {
  int full_width = 0;
  int full_height = 0;
  int width = 0;
  int height = 0;
  /** RGBA, width * height * 4 bytes. */
  std::vector<uint8_t> rgba;
};

/** Reads and decodes `path` the way PrepareProfileIconFromFile does (SDL IO, upright). The error carries the
 *  "can't be opened" words. Runs on a worker thread. */
Roe<ProfilePhotoPreview> DecodeProfilePhotoPreview(const std::string& path, int max_edge = 2048);

/**
 * Load, straighten, crop, scale, and encode an image file for relay profile icon upload.
 * The file is read through SDL's IO (Android's picker hands back content:// addresses only SDL can open) and
 * turned upright by its EXIF orientation. `crop` is the part to keep, in pixels of the upright picture (a rectangle
 * reaching outside is clamped to the picture); without it the largest centred square is kept, so the icon is square
 * either way. With `affine_crop` (the crop page) the picture is instead resampled through it (decode upright ->
 * SampleAffine -> scale -> encode) and `crop` is ignored. Failures the person can act on carry their own words (Error::user): the picture cannot be opened, or
 * it is still too large after scaling.
 */
Roe<EncodedProfileIcon> PrepareProfileIconFromFile(const std::string& path, size_t max_bytes = 512u * 1024u,
                                                   int max_dimension = 256, std::optional<SDL_Rect> crop = std::nullopt,
                                                   std::optional<ProfileIconCrop> affine_crop = std::nullopt);

} // namespace pbr
