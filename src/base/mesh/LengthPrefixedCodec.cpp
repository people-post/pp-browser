#include "base/mesh/LengthPrefixedCodec.h"

#include <cstring>

namespace pbr {

std::vector<uint8_t> EncodeLengthPrefixedFrame(const std::vector<uint8_t>& body) {
  std::vector<uint8_t> frame(8 + body.size());
  uint64_t len = body.size();
  for (int i = 7; i >= 0; --i) {
    frame[static_cast<size_t>(i)] = static_cast<uint8_t>(len & 0xff);
    len >>= 8;
  }
  if (!body.empty()) {
    std::memcpy(frame.data() + 8, body.data(), body.size());
  }
  return frame;
}

uint64_t DecodeLengthPrefixedHeader(const std::vector<uint8_t>& header8) {
  uint64_t len = 0;
  for (size_t i = 0; i < 8 && i < header8.size(); ++i) {
    len = (len << 8) | header8[i];
  }
  return len;
}

} // namespace pbr
