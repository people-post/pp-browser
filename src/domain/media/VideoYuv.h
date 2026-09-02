#pragma once

#include "domain/media/IVideoCodec.h"

#include <cstdint>
#include <vector>

namespace pbr {

/** Convert tightly packed RGB24 or RGBA to I420 (BT.601 limited). */
bool RgbaToI420(const uint8_t* rgba, int width, int height, int stride_bytes, bool has_alpha,
                VideoFrameI420& out);

/** Convert I420 to RGBA8 (opaque alpha=255). */
bool I420ToRgba(const VideoFrameI420& in, VideoFrameRgba& out);

/** Copy RGB24/RGBA rows into a tightly packed RGBA8 frame (alpha forced to 255 when absent). */
bool CopyRgbToRgba(const uint8_t* src, int width, int height, int stride_bytes, bool has_alpha,
                   VideoFrameRgba& out);

/** Rotate RGBA 90° clockwise (out size = HxW). */
bool RotateRgba90Cw(const VideoFrameRgba& in, VideoFrameRgba& out);

/** Rotate RGBA 90° counter-clockwise (out size = HxW). */
bool RotateRgba90Ccw(const VideoFrameRgba& in, VideoFrameRgba& out);

/** Center-crop + nearest-neighbor scale to even out_w×out_h (cover). */
bool ScaleCenterCropRgba(const VideoFrameRgba& in, int out_w, int out_h, VideoFrameRgba& out);

/** Premultiply RGBA in place for RmlUi/GL textures. */
void PremultiplyRgbaInPlace(std::vector<uint8_t>& rgba);

/** Force every alpha byte to 255 (camera XRGB/odd conversions can leave A=0). */
void ForceOpaqueAlphaInPlace(std::vector<uint8_t>& rgba);

/** NV12 (Y plane + interleaved UV) → opaque RGBA8. `y_stride` is bytes per Y row. */
bool Nv12ToRgba(const uint8_t* src, int width, int height, int y_stride, VideoFrameRgba& out);

/** YUY2 packed 4:2:2 → opaque RGBA8. `stride_bytes` is bytes per row. */
bool Yuy2ToRgba(const uint8_t* src, int width, int height, int stride_bytes, VideoFrameRgba& out);

} // namespace pbr
