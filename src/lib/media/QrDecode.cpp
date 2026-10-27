#include "lib/media/QrDecode.h"

#include <quirc.h>
#include <algorithm>
#include <memory>

namespace pbr {
std::optional<std::string> DecodeQr(const VideoFrameRgba& frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
      frame.rgba.size() < static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 4) {
    return std::nullopt;
  }
  std::unique_ptr<quirc, decltype(&quirc_destroy)> decoder(quirc_new(), quirc_destroy);
  // Keep scanning bounded even when a camera ignores the requested capture resolution.
  const int step = std::max(1, (std::max(frame.width, frame.height) + 1279) / 1280);
  const int width = frame.width / step;
  const int height = frame.height / step;
  if (!decoder || width <= 0 || height <= 0 || quirc_resize(decoder.get(), width, height) < 0) {
    return std::nullopt;
  }
  uint8_t* gray = quirc_begin(decoder.get(), nullptr, nullptr);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t i = (static_cast<size_t>(y * step) * static_cast<size_t>(frame.width) +
                        static_cast<size_t>(x * step)) * 4;
      gray[y * width + x] = static_cast<uint8_t>((77 * frame.rgba[i] + 150 * frame.rgba[i + 1] +
                                                29 * frame.rgba[i + 2]) >> 8);
    }
  }
  quirc_end(decoder.get());
  for (int i = 0; i < quirc_count(decoder.get()); ++i) {
    quirc_code code{};
    quirc_data data{};
    quirc_extract(decoder.get(), i, &code);
    auto error = quirc_decode(&code, &data);
    if (error == QUIRC_ERROR_DATA_ECC) {
      quirc_flip(&code);
      error = quirc_decode(&code, &data);
    }
    if (error == QUIRC_SUCCESS) {
      return std::string(reinterpret_cast<const char*>(data.payload), static_cast<size_t>(data.payload_len));
    }
  }
  return std::nullopt;
}
} // namespace pbr
