#include "foundation/platform/ProfileIconImagePrep.h"

#include "foundation/platform/ImageEncode.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int kJpegQualities[] = {85, 70, 55, 40, 30};

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
