#pragma once

#include "lib/media/IVideoCodec.h"
#include <optional>
#include <string>

namespace pbr {
/** First readable QR payload, as raw bytes in a string; never interprets or executes it. */
std::optional<std::string> DecodeQr(const VideoFrameRgba& frame);
} // namespace pbr
