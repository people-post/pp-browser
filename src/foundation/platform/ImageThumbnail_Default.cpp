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

Roe<std::vector<uint8_t>> MakeImageThumbnailJpeg(const std::string& image_path, const int max_dimension) {
  if (image_path.empty() || max_dimension <= 0) {
    return Error("Image path and size are required");
  }
  // The header says the size before anything is decoded; an unreadable header is refused too.
  if (!ImageWithinPixelCap(image_path, kMaxThumbnailSourcePixels)) {
    return Error("Image is too large to thumbnail");
  }
  SDL_Surface* loaded = IMG_Load(image_path.c_str());
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
