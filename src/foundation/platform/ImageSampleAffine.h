#pragma once

#include "foundation/platform/ImageAffine.h"

#include <SDL3/SDL.h>

namespace pbr {

/**
 * Resamples `source` into a new `side` x `side` picture through `affine` (output pixel coordinates -> source pixel
 * coordinates, see ImageAffine): each output pixel's centre (u + 0.5, v + 0.5) is mapped into the source and read
 * bilinearly between the source pixel centres; a coordinate outside the source reads the nearest edge pixel (a turned
 * crop's corners, outside the round avatar, fall off the picture). The result is SDL_PIXELFORMAT_RGBA32 when the
 * source format has alpha and SDL_PIXELFORMAT_RGB24 otherwise, so SurfaceHasAlpha (PNG or JPEG) answers as for the
 * source. Does not consume `source`. Caller owns the result; nullptr when `source` is null or empty, `side` < 1 or an
 * affine coefficient is not finite.
 */
SDL_Surface* SampleAffine(SDL_Surface* source, int side, const ImageAffine& affine);

} // namespace pbr
