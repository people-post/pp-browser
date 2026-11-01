#pragma once

#include "lib/media/IVideoCodec.h"
#include <optional>
#include <string>

namespace pbr {
struct QrRegion {
  float center_x = .5f;
  float center_y = .5f;
  float width = 1.f;
  float height = 1.f;
};
struct QrDetection {
  std::optional<std::string> text;
  /** Normalized bounds of a detected QR, even when its payload is not yet readable. */
  std::optional<QrRegion> region;
};
QrDetection InspectQr(const VideoFrameRgba& frame);
/** First readable QR payload, as raw bytes in a string; never interprets or executes it. */
std::optional<std::string> DecodeQr(const VideoFrameRgba& frame);
} // namespace pbr
