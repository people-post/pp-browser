#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** u64 BE length + UTF-8 JSON framing for libp2p chat-history streams (D060). */
class ChatHistoryStreamCodec {
public:
  static Roe<std::vector<uint8_t>> EncodeFrame(const std::string& json_utf8);
  static Roe<std::string> DecodeFrame(const std::vector<uint8_t>& frame_bytes);
};

} // namespace pbr
