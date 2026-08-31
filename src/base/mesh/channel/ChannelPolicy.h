#pragma once

#include "base/mesh/channel/Types.h"
#include "base/p2p/Libp2pExecutorLimits.h"

#include <chrono>
#include <cstddef>
#include <functional>

namespace pbr::amp {

enum class ChannelDropPolicy {
  Never,
  Oldest,
};

struct ChannelPolicy {
  ChannelClass cls = ChannelClass::Control;
  ChannelDropPolicy drop = ChannelDropPolicy::Never;
  size_t max_outbound_frames = 0;
  bool write_preferred = false;
  bool read_once = false;
  size_t max_message_bytes = Libp2pExecutorLimits::kMaxChatStreamJsonBytes;
  std::chrono::milliseconds read_timeout{0};
  std::function<void()> on_outbound_drop;
};

inline ChannelPolicy ControlJsonChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{8000}) {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Control;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxControlOutboundFrames;
  policy.read_once = true;
  policy.max_message_bytes = Libp2pExecutorLimits::kMaxChatStreamJsonBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

inline ChannelPolicy CallMediaChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Realtime;
  policy.drop = ChannelDropPolicy::Oldest;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxCallMediaOutboundFrames;
  policy.write_preferred = true;
  policy.max_message_bytes = Libp2pExecutorLimits::kMaxCallMediaFrameBytes;
  return policy;
}

/** Reliable hello/teardown leg for `/pp-browser/call-media/1.0.0` (AMP-CHANNEL). */
inline ChannelPolicy CallMediaControlChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::RealtimeControl;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxControlOutboundFrames;
  policy.read_once = false;
  policy.max_message_bytes = Libp2pExecutorLimits::kMaxChatStreamJsonBytes;
  policy.read_timeout = std::chrono::milliseconds{8000};
  return policy;
}

inline ChannelPolicy ChatBlobChannelPolicy(bool read_once = false) {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Bulk;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxControlOutboundFrames;
  policy.read_once = read_once;
  policy.max_message_bytes = Libp2pExecutorLimits::kMaxChatBlobFrameBytes;
  policy.read_timeout = std::chrono::milliseconds{8000};
  return policy;
}

inline ChannelPolicy CapabilityChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Control;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = 1;
  policy.max_message_bytes = 64 * 1024;
  return policy;
}

/**
 * `/pp-browser/circuit-relay/1.0.0` tunnel: JSON bridge handshake then opaque DATA splice.
 * Reliable; not read_once (stays open for forward).
 */
inline ChannelPolicy CircuitTunnelChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{8000}) {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Control;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxControlOutboundFrames;
  policy.read_once = false;
  policy.max_message_bytes = Libp2pExecutorLimits::kMaxChatStreamJsonBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

/** Media-relay hop leg — BestEffort realtime fan-out (AMP-CHANNEL; D7b). */
inline ChannelPolicy MediaRelayHopChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Realtime;
  policy.drop = ChannelDropPolicy::Oldest;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxMediaRelayOutboundFrames;
  policy.write_preferred = true;
  policy.max_message_bytes = Libp2pExecutorLimits::kMaxMediaDataFrameBytes;
  return policy;
}

/** Media-relay client attach leg — Reliable control + attach JSON. */
inline ChannelPolicy MediaRelayClientChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{8000}) {
  ChannelPolicy policy;
  policy.cls = ChannelClass::RealtimeControl;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxMediaRelayClientOutboundFrames;
  policy.read_once = false;
  policy.max_message_bytes = Libp2pExecutorLimits::kMaxChatStreamJsonBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

} // namespace pbr::amp
