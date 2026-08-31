#include "base/messaging/ChatHistoryStreamCodec.h"

#include "base/messaging/MessagingLimits.h"

#include <cstring>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

void AppendU64Be(std::vector<uint8_t>& out, const uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

Roe<uint64_t> ReadU64Be(const std::vector<uint8_t>& bytes, const size_t offset) {
  if (offset + 8 > bytes.size()) {
    return Error("Truncated chat-history frame length");
  }
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value = (value << 8) | bytes[offset + i];
  }
  return value;
}

} // namespace

Roe<std::vector<uint8_t>> ChatHistoryStreamCodec::EncodeFrame(const std::string& json_utf8) {
  if (json_utf8.size() > kMaxRelayEnvelopeJsonBytes) {
    return Error("Chat-history JSON exceeds max size");
  }
  std::vector<uint8_t> frame;
  frame.reserve(8 + json_utf8.size());
  AppendU64Be(frame, json_utf8.size());
  frame.insert(frame.end(), json_utf8.begin(), json_utf8.end());
  return frame;
}

Roe<std::string> ChatHistoryStreamCodec::DecodeFrame(const std::vector<uint8_t>& frame_bytes) {
  if (frame_bytes.size() < 8) {
    return Error("Chat-history frame too short");
  }
  auto length = ReadU64Be(frame_bytes, 0);
  if (!length) {
    return length.error();
  }
  if (*length > kMaxRelayEnvelopeJsonBytes) {
    return Error("Chat-history frame payload exceeds max size");
  }
  if (frame_bytes.size() != 8 + *length) {
    return Error("Chat-history frame length mismatch");
  }
  return std::string(reinterpret_cast<const char*>(frame_bytes.data() + 8), *length);
}

} // namespace pbr
