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
/** `source` laid on white, without alpha (a JPEG has none: what was transparent would come out as whatever colour
 *  lies under the alpha, often black). Consumes `source`; returns it unchanged when it has no alpha or colour key,
 *  nullptr when the copy cannot be made. */
SDL_Surface* FlattenOntoWhite(SDL_Surface* source);
// Returns a new surface whose long edge is <= max_dimension (a copy when already small; never upscales).
// Caller owns the result; nullptr on failure.
SDL_Surface* ScaleToMaxDimension(SDL_Surface* source, int max_dimension);

/** A copy of the `rect` part of `source` as a new surface. A rectangle reaching outside the picture is clamped to it;
 *  nullptr when nothing of it is inside (an empty rectangle included) or the copy cannot be made. Pixels (alpha too)
 *  are copied as they are; an indexed picture comes out as RGBA. `source` is not consumed; caller owns the result. */
SDL_Surface* CropSurface(SDL_Surface* source, SDL_Rect rect);

/** Rotates / mirrors `source` upright for an EXIF orientation 2..8 (any whole-byte pixel format). Consumes
 *  `source`; returns it unchanged for 1 or an unknown value, nullptr when the copy cannot be made. */
SDL_Surface* ApplyExifOrientation(SDL_Surface* source, int orientation);

/** Decodes a picture file's bytes the right way up: SDL_image ignores a JPEG's EXIF orientation, so a phone's portrait
 *  photo came out on its side; it is applied here. `type_hint` is SDL_image's (the file extension, may be empty).
 *  Caller owns the result; nullptr when the bytes cannot be decoded. */
SDL_Surface* DecodeImageUpright(const uint8_t* bytes, size_t size, const char* type_hint);

} // namespace pbr
