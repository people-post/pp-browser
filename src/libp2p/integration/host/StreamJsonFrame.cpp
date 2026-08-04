#include "libp2p/integration/host/StreamJsonFrame.h"

#include "libp2p/integration/host/StreamFrameIo.h"

#include <algorithm>

namespace pbr {

Roe<std::vector<uint8_t>> EncodeStreamJsonFrame(const std::string& json_utf8) {
  if (json_utf8.size() > kMaxStreamJsonFrameBytes) {
    return Error("json frame too large");
  }
  std::vector<uint8_t> frame(8 + json_utf8.size());
  uint64_t len = json_utf8.size();
  for (int i = 7; i >= 0; --i) {
    frame[static_cast<size_t>(i)] = static_cast<uint8_t>(len & 0xff);
    len >>= 8;
  }
  std::copy(json_utf8.begin(), json_utf8.end(), frame.begin() + 8);
  return frame;
}

Roe<std::string> DecodeStreamJsonFrame(const std::vector<uint8_t>& frame_bytes) {
  if (frame_bytes.size() < 8) {
    return Error("json frame truncated");
  }
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame_bytes[i];
  }
  if (payload_len > kMaxStreamJsonFrameBytes || frame_bytes.size() != 8 + payload_len) {
    return Error("json frame length mismatch");
  }
  return std::string(frame_bytes.begin() + 8, frame_bytes.end());
}

Roe<std::string> BlockingReadStreamJson(const std::shared_ptr<libp2p::connection::Stream>& stream) {
  LengthPrefixedFrameConfig config;
  config.max_frame_bytes = kMaxStreamJsonFrameBytes;
  auto body = BlockingReadLengthPrefixedFrame(stream, config);
  if (!body) {
    return body.error();
  }
  return std::string(body->begin(), body->end());
}

Roe<void> BlockingWriteStreamJson(const std::shared_ptr<libp2p::connection::Stream>& stream,
                                  const std::string& json_utf8) {
  if (json_utf8.size() > kMaxStreamJsonFrameBytes) {
    return Error("json frame too large");
  }
  std::vector<uint8_t> body(json_utf8.begin(), json_utf8.end());
  return BlockingWriteLengthPrefixedFrame(stream, body);
}

} // namespace pbr
