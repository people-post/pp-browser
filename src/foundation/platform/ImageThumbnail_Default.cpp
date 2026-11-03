#include "foundation/platform/ImageThumbnail.h"

#include "common/ImageMetadata.h"
#include "foundation/platform/ImageEncode.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr uint64_t kMaxThumbnailSourcePixels = 64'000'000;
constexpr int kThumbnailJpegQuality = 80;

} // namespace

Roe<std::vector<uint8_t>> MakeImageThumbnailJpeg(const std::vector<uint8_t>& image_bytes, const int max_dimension) {
  if (image_bytes.empty() || max_dimension <= 0) {
    return Error("Image bytes and size are required");
  }
  // The header says the size before anything is decoded; an unreadable header is refused too.
  const auto header = ReadImageHeaderBytes(image_bytes);
  if (!header || static_cast<uint64_t>(header->width) * static_cast<uint64_t>(header->height) > kMaxThumbnailSourcePixels) {
    return Error("Image is too large to thumbnail");
  }
  SDL_IOStream* io = SDL_IOFromConstMem(image_bytes.data(), image_bytes.size());
  if (!io) {
    return Error(std::string("Failed to read image: ") + (SDL_GetError() ? SDL_GetError() : "unknown error"));
  }
  SDL_Surface* loaded = IMG_Load_IO(io, /*closeio=*/true);
  if (!loaded) {
    return Error(std::string("Failed to load image: ") + (SDL_GetError() ? SDL_GetError() : "unknown error"));
  }
  SDL_Surface* scaled = ScaleToMaxDimension(loaded, max_dimension);
  SDL_DestroySurface(loaded);
  if (!scaled) {
    return Error("Failed to scale image");
  }
  auto jpeg = EncodeJpegBytes(scaled, kThumbnailJpegQuality);
  SDL_DestroySurface(scaled);
  return jpeg;
}

} // namespace pbr
