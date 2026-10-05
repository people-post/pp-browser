#include "foundation/platform/AiImagePrep.h"

#include "foundation/platform/ImageEncode.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int kJpegQualities[] = {85, 70, 55, 40};
constexpr int kMaxHalvingRounds = 3; // 1 initial attempt + 3 halvings

Error PrepError(const AiImagePrepError kind, const std::string& detail) {
  return Error(static_cast<int32_t>(kind), detail);
}

// Flattens onto white so transparent pixels do not turn black in JPEG. Consumes `source`.
SDL_Surface* FlattenOnWhite(SDL_Surface* source) {
  SDL_Surface* flat = SDL_CreateSurface(source->w, source->h, SDL_PIXELFORMAT_RGB24);
  if (flat) {
    SDL_FillSurfaceRect(flat, nullptr, SDL_MapSurfaceRGB(flat, 255, 255, 255));
    SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_BLEND);
    if (!SDL_BlitSurface(source, nullptr, flat, nullptr)) {
      SDL_DestroySurface(flat);
      flat = nullptr;
    }
  }
  SDL_DestroySurface(source);
  return flat;
}

} // namespace

Roe<PreparedAiImage> PrepareAiImageFromFile(const std::string& path, const size_t max_bytes,
                                            const int max_dimension) {
  if (path.empty()) {
    return PrepError(AiImagePrepError::NotAnImage, "Image path is required");
  }
  SDL_Surface* loaded = IMG_Load(path.c_str());
  if (!loaded) {
    return PrepError(AiImagePrepError::NotAnImage,
                     std::string("Failed to load image: ") + (SDL_GetError() ? SDL_GetError() : "unknown error"));
  }
  SDL_Surface* surface = ScaleToMaxDimension(loaded, max_dimension);
  SDL_DestroySurface(loaded);
  if (!surface) {
    return PrepError(AiImagePrepError::Failed, "Failed to scale image");
  }

  PreparedAiImage prepared;
  if (SurfaceHasAlpha(surface)) {
    auto png = EncodePngBytes(surface);
    if (png && png.value().size() <= max_bytes) {
      prepared.width = surface->w;
      prepared.height = surface->h;
      prepared.bytes = std::move(png.value());
      prepared.mime = "image/png";
      SDL_DestroySurface(surface);
      return prepared;
    }
    surface = FlattenOnWhite(surface);
    if (!surface) {
      return PrepError(AiImagePrepError::Failed, "Failed to flatten image");
    }
  }

  for (int round = 0; round <= kMaxHalvingRounds; ++round) {
    for (const int quality : kJpegQualities) {
      auto jpeg = EncodeJpegBytes(surface, quality);
      if (jpeg && jpeg.value().size() <= max_bytes) {
        prepared.width = surface->w;
        prepared.height = surface->h;
        prepared.bytes = std::move(jpeg.value());
        prepared.mime = "image/jpeg";
        SDL_DestroySurface(surface);
        return prepared;
      }
    }
    if (round == kMaxHalvingRounds) {
      break;
    }
    SDL_Surface* smaller =
        SDL_ScaleSurface(surface, std::max(1, surface->w / 2), std::max(1, surface->h / 2), SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(surface);
    if (!smaller) {
      return PrepError(AiImagePrepError::Failed, "Failed to scale image");
    }
    surface = smaller;
  }
  SDL_DestroySurface(surface);
  return PrepError(AiImagePrepError::TooLarge, "Image is too large after compression");
}

} // namespace pbr
