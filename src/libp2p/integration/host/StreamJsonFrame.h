#pragma once

#include "common/Error.h"

#include <libp2p/connection/stream.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

constexpr size_t kMaxStreamJsonFrameBytes = 64 * 1024;

/** u64-BE length prefix + UTF-8 JSON payload (shared by dial-back, circuit-relay, etc.). */
Roe<std::vector<uint8_t>> EncodeStreamJsonFrame(const std::string& json_utf8);
Roe<std::string> DecodeStreamJsonFrame(const std::vector<uint8_t>& frame_bytes);

/** Blocking JSON read/write for control-plane worker threads. */
Roe<std::string> BlockingReadStreamJson(const std::shared_ptr<libp2p::connection::Stream>& stream);
Roe<void> BlockingWriteStreamJson(const std::shared_ptr<libp2p::connection::Stream>& stream,
                                  const std::string& json_utf8);

} // namespace pbr
