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
  /** Call-media outbound queue cap (StreamIoPolicy / DuplexFrameSession). */
  static constexpr size_t kMaxCallMediaOutboundFrames = 64;
  /** Media-relay hop fanout: one queued frame/peer (latest-wins) + in-flight write. */
  static constexpr size_t kMaxMediaRelayOutboundFrames = 1;
  /** Media-relay client (phone→hop): small queue for subscribe JSON + audio coalesce. */
  static constexpr size_t kMaxMediaRelayClientOutboundFrames = 4;
  /** Control JSON (chat / history): one request or ack queued. */
  static constexpr size_t kMaxControlOutboundFrames = 1;
};

} // namespace pbr
