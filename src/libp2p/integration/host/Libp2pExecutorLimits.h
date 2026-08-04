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
  /** Media-relay binary data frames. */
  static constexpr size_t kMaxMediaDataFrameBytes = 256 * 1024;
  /** Call-media encrypted Opus frames. */
  static constexpr size_t kMaxCallMediaFrameBytes = 16 * 1024;
  /** Call-media outbound queue cap (DuplexFrameSession). */
  static constexpr size_t kMaxCallMediaOutboundFrames = 64;
};

} // namespace pbr
