#pragma once

#include "base/mesh/link/MeshRuntime.h"
#include "base/p2p/CallMediaLegCoordinator.h"
#include "base/p2p/ICallMediaTransport.h"

#include <atomic>
#include <functional>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Blocking Connect facade over CallMediaLegCoordinator for CallLibp2pMediaBridge ([A020]/ [A021]).
 * `io_pump` (MeshHost::Tick) may run on the Connect waiter while MessagingHub also ticks —
 * MeshRuntime serializes Drive/PostToIo.
 */
class CallMediaAmpTransport : public ICallMediaTransport {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = CallMediaLegCoordinator::WorkerPost;

  CallMediaAmpTransport(amp::MeshRuntime& runtime, IoPump io_pump, WorkerPost post_worker = {});
  ~CallMediaAmpTransport() override;

  CallMediaAmpTransport(const CallMediaAmpTransport&) = delete;
  CallMediaAmpTransport& operator=(const CallMediaAmpTransport&) = delete;

  void Start() override;
  void Stop() override;

  void SetInboundHandler(
      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler) override;
  void ClearInboundHandler() override;

  bool IsActive() const override;
  CallMediaDirectConnectParams ActiveParams() const override;
  CallMediaSessionPhase Phase() const override;
  void Detach() override;

  Roe<void> Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    int timeout_ms = 15000) override;

  Roe<void> SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0) override;
  Roe<void> SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                      uint8_t mark = 0) override;

  CallMediaLegCoordinator& Coordinator() { return coordinator_; }

private:
  CallMediaLegId ActiveLegId() const;

  CallMediaLegCoordinator coordinator_;
  IoPump io_pump_;
  mutable std::mutex mu_;
  CallMediaLegId active_leg_{};
  CallMediaDirectConnectParams active_params_;
  std::atomic<bool> started_{false};
};

} // namespace pbr
