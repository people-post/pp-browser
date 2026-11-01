#include "foundation/platform/AiImagePrep.h"

#include "foundation/platform/ImageEncode.h"
#include "foundation/platform/ImageMetadata.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cstring>
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

// Rotates / mirrors `source` upright for an EXIF orientation 2..8, any pixel format with whole bytes per pixel.
// Consumes `source`; returns it unchanged for 1.
SDL_Surface* ApplyExifOrientation(SDL_Surface* source, const int orientation) {
  if (orientation < 2 || orientation > 8) {
    return source;
  }
  const int w = source->w;
  const int h = source->h;
  const bool swap = orientation >= 5;
  SDL_Surface* out = SDL_CreateSurface(swap ? h : w, swap ? w : h, source->format);
  if (!out) {
    SDL_DestroySurface(source);
    return nullptr;
  }
  const int bpp = SDL_BYTESPERPIXEL(source->format);
  for (int y = 0; y < h; ++y) {
    const auto* from = static_cast<const uint8_t*>(source->pixels) + static_cast<size_t>(y) * source->pitch;
    for (int x = 0; x < w; ++x) {
      int dx = x;
      int dy = y;
      switch (orientation) {
      case 2: dx = w - 1 - x; break;
      case 3: dx = w - 1 - x; dy = h - 1 - y; break;
      case 4: dy = h - 1 - y; break;
      case 5: dx = y; dy = x; break;
      case 6: dx = h - 1 - y; dy = x; break;
      case 7: dx = h - 1 - y; dy = w - 1 - x; break;
      case 8: dx = y; dy = w - 1 - x; break;
      default: break;
      }
      auto* to = static_cast<uint8_t*>(out->pixels) + static_cast<size_t>(dy) * out->pitch + static_cast<size_t>(dx) * bpp;
      std::memcpy(to, from + static_cast<size_t>(x) * bpp, static_cast<size_t>(bpp));
    }
  }
  SDL_DestroySurface(source);
  return out;
}

} // namespace

Roe<PreparedAiImage> PrepareAiImageFromFile(const std::string& path, const size_t max_bytes,
                                            const int max_dimension, const ImagePrepOptions& options) {
  if (path.empty()) {
    return PrepError(AiImagePrepError::NotAnImage, "Image path is required");
  }
  int orientation = 1;
  if (options.max_pixels > 0 || options.apply_exif_orientation) {
    const auto header = ReadImageHeader(path);
    if (options.max_pixels > 0) {
      if (!header) {
        return PrepError(AiImagePrepError::NotAnImage, "Image size could not be read");
      }
      // Checked on the file head, so a huge picture is never decoded.
      if (static_cast<uint64_t>(header->width) * static_cast<uint64_t>(header->height) > options.max_pixels) {
        return PrepError(AiImagePrepError::TooLarge, "Image has too many pixels");
      }
    }
    if (header && options.apply_exif_orientation) {
      orientation = header->exif_orientation;
    }
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
  surface = ApplyExifOrientation(surface, orientation);
  if (!surface) {
    return PrepError(AiImagePrepError::Failed, "Failed to rotate image");
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
