#include "foundation/platform/ProfileIconImagePrep.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int kJpegQualities[] = {85, 70, 55, 40, 30};

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
    return Error("Failed to encode profile icon JPEG");
  }
  const Sint64 size = SDL_GetIOSize(io);
  if (size <= 0) {
    SDL_CloseIO(io);
    return Error("Encoded profile icon JPEG is empty");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) < 0) {
    SDL_CloseIO(io);
    return Error("Failed to rewind profile icon JPEG buffer");
  }
  if (SDL_ReadIO(io, bytes.data(), bytes.size()) != static_cast<size_t>(size)) {
    SDL_CloseIO(io);
    return Error("Failed to read profile icon JPEG buffer");
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
    return Error("Failed to encode profile icon PNG");
  }
  const Sint64 size = SDL_GetIOSize(io);
  if (size <= 0) {
    SDL_CloseIO(io);
    return Error("Encoded profile icon PNG is empty");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) < 0) {
    SDL_CloseIO(io);
    return Error("Failed to rewind profile icon PNG buffer");
  }
  if (SDL_ReadIO(io, bytes.data(), bytes.size()) != static_cast<size_t>(size)) {
    SDL_CloseIO(io);
    return Error("Failed to read profile icon PNG buffer");
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

} // namespace

Roe<EncodedProfileIcon> PrepareProfileIconFromFile(const std::string& path, const size_t max_bytes,
                                                    const int max_dimension) {
  if (path.empty()) {
    return Error("Image path is required");
  }
  SDL_Surface* loaded = IMG_Load(path.c_str());
  if (!loaded) {
    return Error(std::string("Failed to load image: ") + (SDL_GetError() ? SDL_GetError() : "unknown error"));
  }

  SDL_Surface* scaled = ScaleToMaxDimension(loaded, max_dimension);
  SDL_DestroySurface(loaded);
  if (!scaled) {
    return Error("Failed to scale profile icon image");
  }

  EncodedProfileIcon prepared;
  if (SurfaceHasAlpha(scaled)) {
    auto png = EncodePngBytes(scaled);
    SDL_DestroySurface(scaled);
    if (!png) {
      return png.error();
    }
    if (png.value().size() > max_bytes) {
      return Error("Profile icon is too large after PNG encoding");
    }
    prepared.bytes = std::move(png.value());
    prepared.content_type = "image/png";
    prepared.kind = "image/png";
    prepared.file_extension = "png";
    return prepared;
  }

  for (const int quality : kJpegQualities) {
    auto jpeg = EncodeJpegBytes(scaled, quality);
    if (!jpeg) {
      continue;
    }
    if (jpeg.value().size() <= max_bytes) {
      prepared.bytes = std::move(jpeg.value());
      prepared.content_type = "image/jpeg";
      prepared.kind = "image/jpeg";
      prepared.file_extension = "jpg";
      SDL_DestroySurface(scaled);
      return prepared;
    }
  }

  SDL_DestroySurface(scaled);
  return Error("Profile icon is too large after JPEG encoding");
}

} // namespace pbr
