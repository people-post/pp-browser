#include "foundation/platform/ImageEncode.h"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>

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

} // namespace pbr
