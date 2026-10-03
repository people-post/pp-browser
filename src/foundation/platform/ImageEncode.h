#pragma once

#include "common/Error.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

// Internal helpers shared by the image-preparation entry points (profile icon, AI image).

bool SurfaceHasAlpha(SDL_Surface* surface);
Roe<std::vector<uint8_t>> EncodeJpegBytes(SDL_Surface* surface, int quality);
Roe<std::vector<uint8_t>> EncodePngBytes(SDL_Surface* surface);
// Returns a new surface whose long edge is <= max_dimension (a copy when already small; never upscales).
// Caller owns the result; nullptr on failure.
SDL_Surface* ScaleToMaxDimension(SDL_Surface* source, int max_dimension);

} // namespace pbr
