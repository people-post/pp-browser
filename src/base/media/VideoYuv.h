#pragma once

#include "base/media/IVideoCodec.h"

#include <cstdint>
#include <vector>

namespace pbr {

/** Convert tightly packed RGB24 or RGBA to I420 (BT.601 limited). */
bool RgbaToI420(const uint8_t* rgba, int width, int height, int stride_bytes, bool has_alpha,
                VideoFrameI420& out);

/** Convert I420 to RGBA8 (opaque alpha=255). */
bool I420ToRgba(const VideoFrameI420& in, VideoFrameRgba& out);

/** Premultiply RGBA in place for RmlUi/GL textures. */
void PremultiplyRgbaInPlace(std::vector<uint8_t>& rgba);

} // namespace pbr
