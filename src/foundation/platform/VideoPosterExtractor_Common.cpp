#include "foundation/platform/VideoPosterExtractor.h"

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
  auto jpeg = EncodeJpegBytes(surface, 80);
  SDL_DestroySurface(surface);
  return jpeg;
}

} // namespace pbr
