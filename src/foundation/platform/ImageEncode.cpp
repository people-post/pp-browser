#include "foundation/platform/ImageEncode.h"

#include "common/ImageMetadata.h"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pbr {

bool SurfaceHasAlpha(SDL_Surface* surface) {
  if (!surface || !surface->format) {
    return false;
  }
  return SDL_ISPIXELFORMAT_ALPHA(surface->format);
}

Roe<std::vector<uint8_t>> EncodeJpegBytes(SDL_Surface* surface, const int quality) {
  SDL_IOStream* io = SDL_IOFromDynamicMem();
  if (!io) {
    return Error("Failed to allocate JPEG buffer");
  }
  if (!IMG_SaveJPG_IO(surface, io, false, quality)) {
    SDL_CloseIO(io);
    return Error("Failed to encode JPEG");
  }
  const Sint64 size = SDL_GetIOSize(io);
  if (size <= 0) {
    SDL_CloseIO(io);
    return Error("Encoded JPEG is empty");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) < 0) {
    SDL_CloseIO(io);
    return Error("Failed to rewind JPEG buffer");
  }
  if (SDL_ReadIO(io, bytes.data(), bytes.size()) != static_cast<size_t>(size)) {
    SDL_CloseIO(io);
    return Error("Failed to read JPEG buffer");
  }
  SDL_CloseIO(io);
  return bytes;
}

Roe<std::vector<uint8_t>> EncodePngBytes(SDL_Surface* surface) {
  SDL_IOStream* io = SDL_IOFromDynamicMem();
  if (!io) {
    return Error("Failed to allocate PNG buffer");
  }
  if (!IMG_SavePNG_IO(surface, io, false)) {
    SDL_CloseIO(io);
    return Error("Failed to encode PNG");
  }
  const Sint64 size = SDL_GetIOSize(io);
  if (size <= 0) {
    SDL_CloseIO(io);
    return Error("Encoded PNG is empty");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) < 0) {
    SDL_CloseIO(io);
    return Error("Failed to rewind PNG buffer");
  }
  if (SDL_ReadIO(io, bytes.data(), bytes.size()) != static_cast<size_t>(size)) {
    SDL_CloseIO(io);
    return Error("Failed to read PNG buffer");
  }
  SDL_CloseIO(io);
  return bytes;
}

SDL_Surface* FlattenOntoWhite(SDL_Surface* source) {
  if (!source || (!SurfaceHasAlpha(source) && !SDL_SurfaceHasColorKey(source))) {
    return source;
  }
  SDL_Surface* out = SDL_CreateSurface(source->w, source->h, SDL_PIXELFORMAT_XRGB8888);
  const bool ok = out && SDL_FillSurfaceRect(out, nullptr, SDL_MapSurfaceRGB(out, 255, 255, 255)) &&
                  SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_BLEND) &&
                  SDL_BlitSurface(source, nullptr, out, nullptr);
  SDL_DestroySurface(source);
  if (!ok) {
    SDL_DestroySurface(out);
    return nullptr;
  }
  return out;
}

SDL_Surface* ScaleToMaxDimension(SDL_Surface* source, const int max_dimension) {
  if (!source) {
    return nullptr;
  }
  const int src_w = source->w;
  const int src_h = source->h;
  if (src_w <= 0 || src_h <= 0) {
    return nullptr;
  }
  const int max_side = std::max(src_w, src_h);
  if (max_side <= max_dimension) {
    return SDL_DuplicateSurface(source);
  }
  const double scale = static_cast<double>(max_dimension) / static_cast<double>(max_side);
  const int dst_w = std::max(1, static_cast<int>(std::lround(src_w * scale)));
  const int dst_h = std::max(1, static_cast<int>(std::lround(src_h * scale)));
  return SDL_ScaleSurface(source, dst_w, dst_h, SDL_SCALEMODE_LINEAR);
}

SDL_Surface* CropSurface(SDL_Surface* source, const SDL_Rect rect) {
  if (!source || source->w <= 0 || source->h <= 0) {
    return nullptr;
  }
  // 64-bit sums: a rectangle with x + w near INT_MAX must not wrap round into the picture.
  const int64_t x0 = std::max<int64_t>(rect.x, 0);
  const int64_t y0 = std::max<int64_t>(rect.y, 0);
  const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(rect.x) + rect.w, source->w);
  const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(rect.y) + rect.h, source->h);
  if (x1 <= x0 || y1 <= y0) {
    return nullptr;
  }
  const SDL_Rect from{static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
  const SDL_PixelFormat format = SDL_ISPIXELFORMAT_INDEXED(source->format) ? SDL_PIXELFORMAT_RGBA32 : source->format;
  SDL_Surface* out = SDL_CreateSurface(from.w, from.h, format);
  if (!out) {
    return nullptr;
  }
  // A plain copy: blending a translucent pixel onto the empty copy would darken it.
  SDL_BlendMode mode = SDL_BLENDMODE_NONE;
  SDL_GetSurfaceBlendMode(source, &mode);
  SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
  const bool copied = SDL_BlitSurface(source, &from, out, nullptr);
  SDL_SetSurfaceBlendMode(source, mode);
  if (!copied) {
    SDL_DestroySurface(out);
    return nullptr;
  }
  return out;
}

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
        case 2:
          dx = w - 1 - x;
          break;
        case 3:
          dx = w - 1 - x;
          dy = h - 1 - y;
          break;
        case 4:
          dy = h - 1 - y;
          break;
        case 5:
          dx = y;
          dy = x;
          break;
        case 6:
          dx = h - 1 - y;
          dy = x;
          break;
        case 7:
          dx = h - 1 - y;
          dy = w - 1 - x;
          break;
        case 8:
          dx = y;
          dy = w - 1 - x;
          break;
        default:
          break;
      }
      auto* to =
          static_cast<uint8_t*>(out->pixels) + static_cast<size_t>(dy) * out->pitch + static_cast<size_t>(dx) * bpp;
      std::memcpy(to, from + static_cast<size_t>(x) * bpp, static_cast<size_t>(bpp));
    }
  }
  SDL_DestroySurface(source);
  return out;
}

SDL_Surface* DecodeImageUpright(const uint8_t* bytes, const size_t size, const char* type_hint) {
  if (!bytes || size == 0) {
    return nullptr;
  }
  SDL_Surface* surface = IMG_LoadTyped_IO(SDL_IOFromConstMem(bytes, size), true, type_hint ? type_hint : "");
  if (!surface) {
    return nullptr;
  }
  // The orientation sits in the head of the file (EXIF comes before the image data); the same window as
  // ReadImageHeader, which sizes the bubble, so a picture is never laid out turned but drawn sideways.
  const std::vector<uint8_t> head(bytes, bytes + std::min(size, kImageHeaderReadBytes));
  const std::optional<ImageHeaderInfo> header = ReadImageHeaderBytes(head);
  return header ? ApplyExifOrientation(surface, header->exif_orientation) : surface;
}

} // namespace pbr
