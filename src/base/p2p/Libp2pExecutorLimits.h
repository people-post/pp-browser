#pragma once

#include <cstddef>

namespace pbr {

/**
 * Documented libp2p integration I/O limits (post executor migration).
 * Services may use tighter caps; these are the shared upper bounds.
 */
struct Libp2pExecutorLimits {
  /** Control-plane JSON (dial-back, circuit-relay RPC). */
  static constexpr size_t kMaxControlJsonFrameBytes = 64 * 1024;
  /** Chat / relay envelope streams (direct chat, chat-history, dial-back). */
  static constexpr size_t kMaxChatStreamJsonBytes = 256 * 1024;
  /** Chat attachment ciphertext (≤ 4 MiB plaintext + AEAD overhead). */
  static constexpr size_t kMaxChatBlobFrameBytes = 4ULL * 1024ULL * 1024ULL + 64 * 1024;
  /** Media-relay binary data frames. */
  static constexpr size_t kMaxMediaDataFrameBytes = 256 * 1024;
  /** Call-media encrypted Opus / H264 video_lo frames (V034; was 16 KiB audio-only). */
  static constexpr size_t kMaxCallMediaFrameBytes = 128 * 1024;
  /** Call-media outbound queue cap (StreamIoPolicy / DuplexFrameSession). */
  static constexpr size_t kMaxCallMediaOutboundFrames = 64;
  /** Media-relay hop fanout: audio + latest video (channel-aware drop keeps ReliableOrdered). */
  static constexpr size_t kMaxMediaRelayOutboundFrames = 2;
  /** Media-relay client (phone→hop): subscribe JSON + audio + IDR headroom. */
  static constexpr size_t kMaxMediaRelayClientOutboundFrames = 6;
  /** Control JSON (chat / history): one request or ack queued. */
  static constexpr size_t kMaxControlOutboundFrames = 1;
};

} // namespace pbr
