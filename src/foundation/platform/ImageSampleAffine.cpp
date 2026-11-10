#include "foundation/platform/ImageSampleAffine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pbr {

SDL_Surface* SampleAffine(SDL_Surface* source, const int side, const ImageAffine& affine) {
  if (!source || source->w <= 0 || source->h <= 0 || side < 1) {
    return nullptr;
  }
  for (const double v : {affine.xu, affine.xv, affine.x0, affine.yu, affine.yv, affine.y0}) {
    if (!std::isfinite(v)) {
      return nullptr;
    }
  }
  const bool alpha = SDL_ISPIXELFORMAT_ALPHA(source->format);
  // RGBA32 and RGB24 sources are read as they are (a JPEG's RGB24 is not copied to RGBA first); anything else is
  // read through one known byte order (R, G, B, A).
  const bool direct = source->format == SDL_PIXELFORMAT_RGBA32 || source->format == SDL_PIXELFORMAT_RGB24;
  SDL_Surface* rgba = direct ? source : SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
  if (!rgba) {
    return nullptr;
  }
  const bool must_lock = SDL_MUSTLOCK(rgba);
  const bool locked_in = must_lock && SDL_LockSurface(rgba);
  SDL_Surface* out = locked_in || !must_lock
                         ? SDL_CreateSurface(side, side, alpha ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_RGB24)
                         : nullptr;
  if (!out) { // the lock failed (pixels are not readable) or no memory
    if (locked_in) {
      SDL_UnlockSurface(rgba);
    }
    if (rgba != source) {
      SDL_DestroySurface(rgba);
    }
    return nullptr;
  }
  const int sw = rgba->w;
  const int sh = rgba->h;
  const int out_bpp = alpha ? 4 : 3;
  const size_t src_bpp = rgba->format == SDL_PIXELFORMAT_RGB24 ? 3 : 4;
  const auto* src = static_cast<const uint8_t*>(rgba->pixels);
  const auto texel = [&](const int x, const int y) {
    return src + static_cast<size_t>(y) * rgba->pitch + static_cast<size_t>(x) * src_bpp;
  };
  for (int v = 0; v < side; ++v) {
    auto* row = static_cast<uint8_t*>(out->pixels) + static_cast<size_t>(v) * out->pitch;
    const double vc = v + 0.5;
    for (int u = 0; u < side; ++u) {
      const double uc = u + 0.5;
      // Source pixel-centre coordinates, clamped to the edge pixels' centres.
      const double sx = std::clamp(affine.xu * uc + affine.xv * vc + affine.x0 - 0.5, 0.0, sw - 1.0);
      const double sy = std::clamp(affine.yu * uc + affine.yv * vc + affine.y0 - 0.5, 0.0, sh - 1.0);
      const int x0 = static_cast<int>(sx);
      const int y0 = static_cast<int>(sy);
      const int x1 = std::min(x0 + 1, sw - 1);
      const int y1 = std::min(y0 + 1, sh - 1);
      const double fx = sx - x0;
      const double fy = sy - y0;
      const uint8_t* p00 = texel(x0, y0);
      const uint8_t* p10 = texel(x1, y0);
      const uint8_t* p01 = texel(x0, y1);
      const uint8_t* p11 = texel(x1, y1);
      uint8_t* to = row + static_cast<size_t>(u) * out_bpp;
      if (!alpha || (p00[3] == 255 && p10[3] == 255 && p01[3] == 255 && p11[3] == 255)) {
        for (int ch = 0; ch < out_bpp; ++ch) {
          const double top = p00[ch] + (p10[ch] - p00[ch]) * fx;
          const double bottom = p01[ch] + (p11[ch] - p01[ch]) * fx;
          to[ch] = static_cast<uint8_t>(std::clamp(std::lround(top + (bottom - top) * fy), 0L, 255L));
        }
        continue;
      }
      // Straight RGBA: a transparent pixel's hidden colour must not bleed into its neighbours, so the colour is
      // averaged weighted by alpha and divided by the averaged alpha.
      const double a00 = (1.0 - fx) * (1.0 - fy) * p00[3];
      const double a10 = fx * (1.0 - fy) * p10[3];
      const double a01 = (1.0 - fx) * fy * p01[3];
      const double a11 = fx * fy * p11[3];
      const double a = a00 + a10 + a01 + a11;
      if (a <= 0.0) {
        to[0] = to[1] = to[2] = to[3] = 0;
        continue;
      }
      for (int ch = 0; ch < 3; ++ch) {
        const double c = (a00 * p00[ch] + a10 * p10[ch] + a01 * p01[ch] + a11 * p11[ch]) / a;
        to[ch] = static_cast<uint8_t>(std::clamp(std::lround(c), 0L, 255L));
      }
      to[3] = static_cast<uint8_t>(std::clamp(std::lround(a), 0L, 255L));
    }
  }
  if (locked_in) {
    SDL_UnlockSurface(rgba);
  }
  if (rgba != source) {
    SDL_DestroySurface(rgba);
  }
  return out;
}

} // namespace pbr
