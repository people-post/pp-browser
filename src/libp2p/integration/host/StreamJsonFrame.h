#pragma once

#include "common/Error.h"
#include "libp2p/integration/host/StreamFrameIo.h"

#include <libp2p/connection/stream.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

constexpr size_t kMaxStreamJsonFrameBytes = 64 * 1024;

/** u64-BE length prefix + UTF-8 JSON payload (shared by dial-back, circuit-relay, etc.). */
Roe<std::vector<uint8_t>> EncodeStreamJsonFrame(const std::string& json_utf8);
Roe<std::string> DecodeStreamJsonFrame(const std::vector<uint8_t>& frame_bytes);

/** Blocking JSON read/write for control-plane worker threads. */
Roe<std::string> BlockingReadStreamJson(
    const std::shared_ptr<libp2p::connection::Stream>& stream,
    size_t max_frame_bytes = kMaxStreamJsonFrameBytes,
    std::chrono::milliseconds read_timeout = kDefaultControlFrameReadTimeout);

Roe<void> BlockingWriteStreamJson(const std::shared_ptr<libp2p::connection::Stream>& stream,
                                  const std::string& json_utf8,
                                  size_t max_frame_bytes = kMaxStreamJsonFrameBytes);

using StreamJsonReadCallback = std::function<void(Roe<std::string>)>;
using StreamJsonWriteCallback = std::function<void(Roe<void>)>;

/**
 * One-shot async JSON frame IO. Completions run on the stream's io_context.
 * Prefer these for call-media hello/ack so Detach can cancel without pinning WorkerPool.
 * read_timeout requires timer_executor (via LengthPrefixedFrameConfig) — use the overload
 * that takes a full config when enabling async deadlines.
 */
void AsyncReadStreamJson(std::shared_ptr<libp2p::connection::Stream> stream,
                         StreamJsonReadCallback on_done, StreamCancelCheck is_cancelled = {},
                         size_t max_frame_bytes = kMaxStreamJsonFrameBytes);

void AsyncReadStreamJson(std::shared_ptr<libp2p::connection::Stream> stream,
                         StreamJsonReadCallback on_done, StreamCancelCheck is_cancelled,
                         LengthPrefixedFrameConfig config);

void AsyncWriteStreamJson(std::shared_ptr<libp2p::connection::Stream> stream, std::string json_utf8,
                          StreamJsonWriteCallback on_done,
                          size_t max_frame_bytes = kMaxStreamJsonFrameBytes);

} // namespace pbr
