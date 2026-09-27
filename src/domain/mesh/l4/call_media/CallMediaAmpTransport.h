#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/l4/call_media/CallMediaLegCoordinator.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Amp call-media transport over CallMediaLegCoordinator ([A020]/ [A021]).
 * Prefer ConnectAsync — MeshHost MeshPump drives Amp; sync Connect is for tests/harnesses.
 */
class CallMediaAmpTransport : public ICallMediaTransport {
public:
  using IoPump = std::function<void()>;

  CallMediaAmpTransport(pp::amp::MeshRuntime& runtime, IoPump io_pump);
  ~CallMediaAmpTransport() override;

  CallMediaAmpTransport(const CallMediaAmpTransport&) = delete;
  CallMediaAmpTransport& operator=(const CallMediaAmpTransport&) = delete;

  void Start() override;
  void Stop() override;

  void SetInboundHandler(CallMediaInboundHandler handler) override;
  void ClearInboundHandler() override;

  bool IsActive() const override;
  CallMediaDirectConnectParams ActiveParams() const override;
  CallMediaSessionPhase Phase() const override;
  CallMediaLinkKind ActiveLinkKind() const override;
  void MigrateTo(CallMediaLinkKind kind, std::function<void(Roe<void>)> done) override;
  void Detach() override;

  void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    std::function<void(Roe<void>)> on_done, int timeout_ms = 15000) override;

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
  /** Answers may arrive after this transport is gone (they are posted); they skip its state then. */
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace pbr
