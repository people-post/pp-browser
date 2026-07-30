#include "base/media/VideoYuv.h"

#include <algorithm>
#include <cmath>

namespace pbr {
namespace {

inline uint8_t ClampU8(int v) {
  return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

} // namespace

bool RgbaToI420(const uint8_t* rgba, const int width, const int height, const int stride_bytes,
                const bool has_alpha, VideoFrameI420& out) {
  if (!rgba || width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
    return false;
  }
  const int bpp = has_alpha ? 4 : 3;
  if (stride_bytes < width * bpp) {
    return false;
  }
  out.width = width;
  out.height = height;
  out.y.assign(static_cast<size_t>(width * height), 0);
  out.u.assign(static_cast<size_t>((width / 2) * (height / 2)), 0);
  out.v.assign(static_cast<size_t>((width / 2) * (height / 2)), 0);

  for (int y = 0; y < height; ++y) {
    const uint8_t* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes);
    for (int x = 0; x < width; ++x) {
      const uint8_t* px = row + x * bpp;
      const int r = px[0];
      const int g = px[1];
      const int b = px[2];
      out.y[static_cast<size_t>(y * width + x)] =
          ClampU8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    }
  }
  for (int y = 0; y < height; y += 2) {
    for (int x = 0; x < width; x += 2) {
      int r_sum = 0;
      int g_sum = 0;
      int b_sum = 0;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const uint8_t* px =
              rgba + static_cast<size_t>(y + dy) * static_cast<size_t>(stride_bytes) + (x + dx) * bpp;
          r_sum += px[0];
          g_sum += px[1];
          b_sum += px[2];
        }
      }
      const int r = r_sum / 4;
      const int g = g_sum / 4;
      const int b = b_sum / 4;
      const size_t uv = static_cast<size_t>((y / 2) * (width / 2) + (x / 2));
      out.u[uv] = ClampU8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
      out.v[uv] = ClampU8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    }
  }
  return true;
}

bool I420ToRgba(const VideoFrameI420& in, VideoFrameRgba& out) {
  if (in.width <= 0 || in.height <= 0 || in.y.empty() || in.u.empty() || in.v.empty()) {
    return false;
  }
  const int width = in.width;
  const int height = in.height;
  out.width = width;
  out.height = height;
  out.rgba.assign(static_cast<size_t>(width * height * 4), 255);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int Y = static_cast<int>(in.y[static_cast<size_t>(y * width + x)]) - 16;
      const int U = static_cast<int>(in.u[static_cast<size_t>((y / 2) * (width / 2) + (x / 2))]) - 128;
      const int V = static_cast<int>(in.v[static_cast<size_t>((y / 2) * (width / 2) + (x / 2))]) - 128;
      const int C = 298 * Y + 128;
      const int r = (C + 409 * V) >> 8;
      const int g = (C - 100 * U - 208 * V) >> 8;
      const int b = (C + 516 * U) >> 8;
      const size_t i = static_cast<size_t>((y * width + x) * 4);
      out.rgba[i + 0] = ClampU8(r);
      out.rgba[i + 1] = ClampU8(g);
      out.rgba[i + 2] = ClampU8(b);
      out.rgba[i + 3] = 255;
    }
  }
  return true;
}

void PremultiplyRgbaInPlace(std::vector<uint8_t>& rgba) {
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const uint8_t a = rgba[i + 3];
    rgba[i + 0] = static_cast<uint8_t>((rgba[i + 0] * a + 127) / 255);
    rgba[i + 1] = static_cast<uint8_t>((rgba[i + 1] * a + 127) / 255);
    rgba[i + 2] = static_cast<uint8_t>((rgba[i + 2] * a + 127) / 255);
  }
}

} // namespace pbr
