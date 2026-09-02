#include "domain/media/VideoYuv.h"

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

bool CopyRgbToRgba(const uint8_t* src, const int width, const int height, const int stride_bytes,
                   const bool has_alpha, VideoFrameRgba& out) {
  if (!src || width <= 0 || height <= 0) {
    return false;
  }
  const int bpp = has_alpha ? 4 : 3;
  if (stride_bytes < width * bpp) {
    return false;
  }
  out.width = width;
  out.height = height;
  out.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes);
    uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
    for (int x = 0; x < width; ++x) {
      const uint8_t* px = row + x * bpp;
      dst[x * 4 + 0] = px[0];
      dst[x * 4 + 1] = px[1];
      dst[x * 4 + 2] = px[2];
      dst[x * 4 + 3] = has_alpha ? px[3] : 255;
    }
  }
  return true;
}

bool RotateRgba90Cw(const VideoFrameRgba& in, VideoFrameRgba& out) {
  if (in.width <= 0 || in.height <= 0 ||
      in.rgba.size() < static_cast<size_t>(in.width) * static_cast<size_t>(in.height) * 4) {
    return false;
  }
  out.width = in.height;
  out.height = in.width;
  out.rgba.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4);
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      const int src_x = y;
      const int src_y = in.height - 1 - x;
      const size_t si = (static_cast<size_t>(src_y) * static_cast<size_t>(in.width) +
                         static_cast<size_t>(src_x)) *
                        4;
      const size_t di =
          (static_cast<size_t>(y) * static_cast<size_t>(out.width) + static_cast<size_t>(x)) * 4;
      out.rgba[di + 0] = in.rgba[si + 0];
      out.rgba[di + 1] = in.rgba[si + 1];
      out.rgba[di + 2] = in.rgba[si + 2];
      out.rgba[di + 3] = in.rgba[si + 3];
    }
  }
  return true;
}

bool RotateRgba90Ccw(const VideoFrameRgba& in, VideoFrameRgba& out) {
  if (in.width <= 0 || in.height <= 0 ||
      in.rgba.size() < static_cast<size_t>(in.width) * static_cast<size_t>(in.height) * 4) {
    return false;
  }
  out.width = in.height;
  out.height = in.width;
  out.rgba.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4);
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      const int src_x = in.width - 1 - y;
      const int src_y = x;
      const size_t si = (static_cast<size_t>(src_y) * static_cast<size_t>(in.width) +
                         static_cast<size_t>(src_x)) *
                        4;
      const size_t di =
          (static_cast<size_t>(y) * static_cast<size_t>(out.width) + static_cast<size_t>(x)) * 4;
      out.rgba[di + 0] = in.rgba[si + 0];
      out.rgba[di + 1] = in.rgba[si + 1];
      out.rgba[di + 2] = in.rgba[si + 2];
      out.rgba[di + 3] = in.rgba[si + 3];
    }
  }
  return true;
}

bool ScaleCenterCropRgba(const VideoFrameRgba& in, int out_w, int out_h, VideoFrameRgba& out) {
  out_w = out_w > 0 ? (out_w & ~1) : 0;
  out_h = out_h > 0 ? (out_h & ~1) : 0;
  if (out_w < 2 || out_h < 2 || in.width <= 0 || in.height <= 0 ||
      in.rgba.size() < static_cast<size_t>(in.width) * static_cast<size_t>(in.height) * 4) {
    return false;
  }
  if (in.width == out_w && in.height == out_h) {
    out = in;
    return true;
  }

  const double scale =
      std::max(static_cast<double>(out_w) / static_cast<double>(in.width),
               static_cast<double>(out_h) / static_cast<double>(in.height));
  const double src_w = static_cast<double>(out_w) / scale;
  const double src_h = static_cast<double>(out_h) / scale;
  const double src_x0 = (static_cast<double>(in.width) - src_w) * 0.5;
  const double src_y0 = (static_cast<double>(in.height) - src_h) * 0.5;

  out.width = out_w;
  out.height = out_h;
  out.rgba.resize(static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 4);
  for (int y = 0; y < out_h; ++y) {
    const int sy = std::clamp(static_cast<int>(src_y0 + (static_cast<double>(y) + 0.5) / scale), 0,
                              in.height - 1);
    for (int x = 0; x < out_w; ++x) {
      const int sx = std::clamp(static_cast<int>(src_x0 + (static_cast<double>(x) + 0.5) / scale), 0,
                                in.width - 1);
      const size_t si =
          (static_cast<size_t>(sy) * static_cast<size_t>(in.width) + static_cast<size_t>(sx)) * 4;
      const size_t di =
          (static_cast<size_t>(y) * static_cast<size_t>(out_w) + static_cast<size_t>(x)) * 4;
      out.rgba[di + 0] = in.rgba[si + 0];
      out.rgba[di + 1] = in.rgba[si + 1];
      out.rgba[di + 2] = in.rgba[si + 2];
      out.rgba[di + 3] = in.rgba[si + 3];
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

void ForceOpaqueAlphaInPlace(std::vector<uint8_t>& rgba) {
  for (size_t i = 3; i < rgba.size(); i += 4) {
    rgba[i] = 255;
  }
}

bool Nv12ToRgba(const uint8_t* src, const int width, const int height, const int y_stride,
                VideoFrameRgba& out) {
  if (!src || width < 2 || height < 2 || (width & 1) || (height & 1) || y_stride < width) {
    return false;
  }
  const uint8_t* y_plane = src;
  const uint8_t* uv_plane = src + static_cast<size_t>(y_stride) * static_cast<size_t>(height);
  const int uv_stride = y_stride;
  out.width = width;
  out.height = height;
  out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 255);
  for (int y = 0; y < height; ++y) {
    const uint8_t* y_row = y_plane + static_cast<size_t>(y) * static_cast<size_t>(y_stride);
    const uint8_t* uv_row =
        uv_plane + static_cast<size_t>(y / 2) * static_cast<size_t>(uv_stride);
    uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
    for (int x = 0; x < width; ++x) {
      const int Y = static_cast<int>(y_row[x]) - 16;
      const int U = static_cast<int>(uv_row[(x & ~1) + 0]) - 128;
      const int V = static_cast<int>(uv_row[(x & ~1) + 1]) - 128;
      const int C = 298 * Y + 128;
      const size_t i = static_cast<size_t>(x) * 4;
      dst[i + 0] = ClampU8((C + 409 * V) >> 8);
      dst[i + 1] = ClampU8((C - 100 * U - 208 * V) >> 8);
      dst[i + 2] = ClampU8((C + 516 * U) >> 8);
      dst[i + 3] = 255;
    }
  }
  return true;
}

bool Yuy2ToRgba(const uint8_t* src, const int width, const int height, const int stride_bytes,
                VideoFrameRgba& out) {
  if (!src || width < 2 || height < 1 || (width & 1) || stride_bytes < width * 2) {
    return false;
  }
  out.width = width;
  out.height = height;
  out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 255);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes);
    uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
    for (int x = 0; x < width; x += 2) {
      const uint8_t* p = row + x * 2;
      const int Y0 = static_cast<int>(p[0]) - 16;
      const int U = static_cast<int>(p[1]) - 128;
      const int Y1 = static_cast<int>(p[2]) - 16;
      const int V = static_cast<int>(p[3]) - 128;
      const int C0 = 298 * Y0 + 128;
      const int C1 = 298 * Y1 + 128;
      const size_t i0 = static_cast<size_t>(x) * 4;
      const size_t i1 = static_cast<size_t>(x + 1) * 4;
      dst[i0 + 0] = ClampU8((C0 + 409 * V) >> 8);
      dst[i0 + 1] = ClampU8((C0 - 100 * U - 208 * V) >> 8);
      dst[i0 + 2] = ClampU8((C0 + 516 * U) >> 8);
      dst[i0 + 3] = 255;
      dst[i1 + 0] = ClampU8((C1 + 409 * V) >> 8);
      dst[i1 + 1] = ClampU8((C1 - 100 * U - 208 * V) >> 8);
      dst[i1 + 2] = ClampU8((C1 + 516 * U) >> 8);
      dst[i1 + 3] = 255;
    }
  }
  return true;
}

} // namespace pbr
