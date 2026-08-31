#pragma once

#include "base/p2p/CallMediaDirectService.h"
#include "base/mesh/link/PeerLinkManager.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * `/pp-browser/call-media/1.0.0` over AMP ChannelSession (Reliable hello + BestEffort media).
 * Parallel stack for migration — production still uses CallMediaDirectService ([A020]).
 */
class AmpCallMediaDirectService {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  using InboundHandler = std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)>;

  AmpCallMediaDirectService(amp::PeerLinkManager& links, IoPump io_pump,
                            WorkerPost post_worker = {});
  ~AmpCallMediaDirectService();

  AmpCallMediaDirectService(const AmpCallMediaDirectService&) = delete;
  AmpCallMediaDirectService& operator=(const AmpCallMediaDirectService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  void SetInboundHandler(InboundHandler handler);
  void ClearInboundHandler();

  bool IsActive() const;
  CallMediaDirectConnectParams ActiveParams() const;
  CallMediaSessionPhase Phase() const;
  void Detach();

  Roe<void> Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks, int timeout_ms = 15000);
  Roe<void> SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0);
  Roe<void> SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq, uint8_t mark = 0);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
