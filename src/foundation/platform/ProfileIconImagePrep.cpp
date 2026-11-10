#include "foundation/platform/ProfileIconImagePrep.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/ImageEncode.h"
#include "foundation/platform/ImageSampleAffine.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int kJpegQualities[] = {85, 70, 55, 40, 30};

Error Unreadable(const std::string& detail) {
  return Error(detail).WithUser(Tr("settings.profile.photo_unreadable"));
}

Error TooLarge(const std::string& detail) {
  return Error(detail).WithUser(Tr("settings.profile.photo_too_large"));
}

/** The file extension in lower case, as SDL_image's type hint ("" for a content:// address without one). */
std::string TypeHint(const std::string& path) {
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos || path.find('/', dot) != std::string::npos || path.size() - dot > 6) {
    return {};
  }
  std::string hint = path.substr(dot + 1);
  std::transform(hint.begin(), hint.end(), hint.begin(), [](unsigned char c) { return std::tolower(c); });
  return hint;
}

/** Reads and decodes `path` upright; the caller owns the surface. */
Roe<SDL_Surface*> LoadUpright(const std::string& path) {
  size_t size = 0;
  void* file = SDL_LoadFile(path.c_str(), &size);
  if (!file) {
    return Unreadable(std::string("Failed to read image: ") + (SDL_GetError() ? SDL_GetError() : "unknown error"));
  }
  SDL_Surface* upright = DecodeImageUpright(static_cast<const uint8_t*>(file), size, TypeHint(path).c_str());
  SDL_free(file);
  if (!upright) {
    return Unreadable(std::string("Failed to decode image: ") + (SDL_GetError() ? SDL_GetError() : "unknown error"));
  }
  return upright;
}

} // namespace

Roe<ProfilePhotoPreview> DecodeProfilePhotoPreview(const std::string& path, const int max_edge) {
  if (path.empty()) {
    return Error("Image path is required");
  }
  auto upright = LoadUpright(path);
  if (!upright) {
    return upright.error();
  }
  ProfilePhotoPreview preview;
  preview.full_width = upright.value()->w;
  preview.full_height = upright.value()->h;
  SDL_Surface* scaled = ScaleToMaxDimension(upright.value(), max_edge);
  SDL_DestroySurface(upright.value());
  if (!scaled) {
    return Unreadable("Failed to scale the preview");
  }
  SDL_Surface* rgba = SDL_ConvertSurface(scaled, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(scaled);
  if (!rgba) {
    return Unreadable("Failed to convert the preview");
  }
  preview.width = rgba->w;
  preview.height = rgba->h;
  preview.rgba.resize(static_cast<size_t>(rgba->w) * rgba->h * 4);
  for (int y = 0; y < rgba->h; ++y) {
    std::copy_n(static_cast<const uint8_t*>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch,
                static_cast<size_t>(rgba->w) * 4, preview.rgba.begin() + static_cast<size_t>(y) * rgba->w * 4);
  }
  SDL_DestroySurface(rgba);
  return preview;
}

Roe<EncodedProfileIcon> PrepareProfileIconFromFile(const std::string& path, const size_t max_bytes,
                                                   const int max_dimension, const std::optional<SDL_Rect> crop,
                                                   const std::optional<ProfileIconCrop> affine_crop) {
  if (path.empty()) {
    return Error("Image path is required");
  }
  auto loaded = LoadUpright(path);
  if (!loaded) {
    return loaded.error();
  }
  SDL_Surface* upright = loaded.value();

  SDL_Surface* cropped = nullptr;
  if (affine_crop) {
    cropped = SampleAffine(upright, affine_crop->side, affine_crop->affine);
  } else {
    const int side = std::min(upright->w, upright->h);
    const SDL_Rect area = crop ? *crop : SDL_Rect{(upright->w - side) / 2, (upright->h - side) / 2, side, side};
    cropped = CropSurface(upright, area);
  }
  SDL_DestroySurface(upright);
  if (!cropped) {
    return Unreadable("Profile icon crop is outside the image");
  }

  SDL_Surface* scaled = ScaleToMaxDimension(cropped, max_dimension);
  SDL_DestroySurface(cropped);
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
      return TooLarge("Profile icon is too large after PNG encoding");
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
  return TooLarge("Profile icon is too large after JPEG encoding");
}

} // namespace pbr
