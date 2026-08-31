#include "base/platform/VideoPosterExtractor.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

Roe<std::vector<uint8_t>> EncodeJpegBytes(SDL_Surface* surface, const int quality) {
  SDL_IOStream* io = SDL_IOFromDynamicMem();
  if (!io) {
    return Error("Failed to allocate JPEG buffer");
  }
  if (!IMG_SaveJPG_IO(surface, io, false, quality)) {
    SDL_CloseIO(io);
    return Error("Failed to encode video poster JPEG");
  }
  const Sint64 size = SDL_GetIOSize(io);
  if (size <= 0) {
    SDL_CloseIO(io);
    return Error("Encoded video poster JPEG is empty");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) < 0) {
    SDL_CloseIO(io);
    return Error("Failed to rewind video poster JPEG buffer");
  }
  if (SDL_ReadIO(io, bytes.data(), bytes.size()) != static_cast<size_t>(size)) {
    SDL_CloseIO(io);
    return Error("Failed to read video poster JPEG buffer");
  }
  SDL_CloseIO(io);
  return bytes;
}

void FillPlayTriangle(SDL_Surface* surface) {
  if (!surface || surface->w <= 0 || surface->h <= 0) {
    return;
  }
  if (!SDL_LockSurface(surface)) {
    return;
  }
  const int w = surface->w;
  const int h = surface->h;
  const int cx = w / 2;
  const int cy = h / 2;
  const int tri_h = std::max(12, h / 4);
  const int tri_w = std::max(10, (tri_h * 5) / 6);
  const int left = cx - tri_w / 3;
  const int right = left + tri_w;
  const int top = cy - tri_h / 2;
  const int bottom = cy + tri_h / 2;
  const Uint32 color = SDL_MapSurfaceRGB(surface, 220, 220, 220);
  const int bpp = SDL_BYTESPERPIXEL(surface->format);
  auto* pixels = static_cast<uint8_t*>(surface->pixels);
  for (int y = top; y <= bottom; ++y) {
    if (y < 0 || y >= h) {
      continue;
    }
    const float t = static_cast<float>(y - top) / static_cast<float>(std::max(1, bottom - top));
    const float edge = t <= 0.5f ? (t * 2.0f) : ((1.0f - t) * 2.0f);
    const int x0 = left;
    const int x1 = left + static_cast<int>(std::lround(static_cast<float>(tri_w) * edge));
    for (int x = x0; x <= x1 && x < right; ++x) {
      if (x < 0 || x >= w) {
        continue;
      }
      uint8_t* p = pixels + static_cast<size_t>(y) * static_cast<size_t>(surface->pitch) +
                   static_cast<size_t>(x) * static_cast<size_t>(bpp);
      if (bpp == 4) {
        *reinterpret_cast<Uint32*>(p) = color;
      } else if (bpp == 3) {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        p[0] = 220;
        p[1] = 220;
        p[2] = 220;
#else
        p[0] = 220;
        p[1] = 220;
        p[2] = 220;
#endif
      }
    }
  }
  SDL_UnlockSurface(surface);
}

} // namespace

Roe<std::vector<uint8_t>> SoftVideoPosterJpeg(const int max_dimension) {
  const int dim = std::max(16, max_dimension);
  int width = dim;
  int height = (dim * 9) / 16;
  if (height < 1) {
    height = 1;
  }
  if (height > dim) {
    height = dim;
    width = (dim * 16) / 9;
    if (width < 1) {
      width = 1;
    }
  }

  SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
  if (!surface) {
    return Error("Failed to create soft video poster surface");
  }
  const Uint32 fill = SDL_MapSurfaceRGB(surface, 0x1a, 0x1a, 0x1a);
  if (!SDL_FillSurfaceRect(surface, nullptr, fill)) {
    SDL_DestroySurface(surface);
    return Error("Failed to fill soft video poster surface");
  }
  FillPlayTriangle(surface);
  auto jpeg = EncodeJpegBytes(surface, 80);
  SDL_DestroySurface(surface);
  return jpeg;
}

} // namespace pbr
