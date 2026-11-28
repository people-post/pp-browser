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
/** The longest edge the decoder looks at: a bigger frame is sampled down to it. Phones keep the 1280 they were measured at. */
constexpr int kQrMobileDecodeEdge = 1280;
/** The desktop's 1080p capture stays whole, so the dense link codes keep their pixels per module. */
constexpr int kQrDesktopDecodeEdge = 1920;
QrDetection InspectQr(const VideoFrameRgba& frame, int max_edge = kQrMobileDecodeEdge);
/** First readable QR payload, as raw bytes in a string; never interprets or executes it. */
std::optional<std::string> DecodeQr(const VideoFrameRgba& frame, int max_edge = kQrMobileDecodeEdge);
} // namespace pbr
