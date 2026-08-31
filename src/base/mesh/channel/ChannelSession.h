#pragma once

#include "base/mesh/channel/ChannelMux.h"
#include "base/mesh/channel/ChannelPolicy.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

namespace pbr::amp {

/**
 * Single-channel L3 pipe (io-thread affine) — AMP counterpart to DuplexFrameSession.
 */
class ChannelSession {
public:
  using FrameHandler = std::function<bool(Roe<std::vector<uint8_t>> body)>;
  using ClosedCallback = std::function<void(const char* reason)>;

  void Bind(ChannelMux& mux, uint32_t channel_id, ChannelPolicy policy, FrameHandler on_frame,
            ClosedCallback on_closed = {});

  /** Queue an L4 payload. Returns false if session closed or queue full (drop policy applied). */
  bool EnqueueOutbound(std::vector<uint8_t> body);

  void Close();
  /** Close the mux channel without invoking `on_closed` (provisional / abandoned roles). */
  void CloseQuiet();
  void Reset(uint32_t code = 1);

  uint32_t ChannelId() const { return channel_id_; }
  size_t OutboundBacklog() const { return outbound_.size() + (write_inflight_ ? 1 : 0); }
  bool IsClosed() const { return closed_; }

private:
  void PumpWrite();
  void FailOutbound(const Error& error);
  void NotifyRemoteTerminal(const char* reason);

  ChannelMux* mux_ = nullptr;
  uint32_t channel_id_ = 0;
  ChannelPolicy policy_;
  FrameHandler on_frame_;
  ClosedCallback on_closed_;
  std::deque<std::vector<uint8_t>> outbound_;
  bool write_inflight_ = false;
  bool closed_ = false;
};

} // namespace pbr::amp
