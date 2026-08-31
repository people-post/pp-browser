#pragma once

#include "base/mesh/link/MeshRuntime.h"
#include "base/p2p/CallMediaDirectService.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

struct CallMediaLegId {
  uint64_t value = 0;
  explicit operator bool() const { return value != 0; }
};

enum class CallMediaLegPhase {
  Closed = 0,
  ControlHello,
  AwaitingMedia,
  MediaReady,
};

/**
 * Non-blocking `/pp-browser/call-media/1.0.0` over AMP channel bundles on MeshRuntime.
 * Parallel stack for migration — production still uses CallMediaDirectService ([A020]).
 */
class CallMediaLegCoordinator {
public:
  using WorkerPost = std::function<void(std::function<void()>)>;
  using LegFinished = std::function<void(Roe<void> result)>;
  using InboundHandler = std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)>;

  CallMediaLegCoordinator(amp::MeshRuntime& runtime, WorkerPost post_worker = {});
  ~CallMediaLegCoordinator();

  CallMediaLegCoordinator(const CallMediaLegCoordinator&) = delete;
  CallMediaLegCoordinator& operator=(const CallMediaLegCoordinator&) = delete;

  void Start();
  void Stop();

  void SetInboundHandler(InboundHandler handler);
  void ClearInboundHandler();

  /** Returns leg id immediately; completion via `on_finished` when MediaReady or error. */
  CallMediaLegId StartLeg(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                          LegFinished on_finished = {}, int timeout_ms = 15000);

  void CancelLeg(CallMediaLegId id);
  void DetachLeg(CallMediaLegId id);

  bool IsLegActive(CallMediaLegId id) const;
  CallMediaLegPhase LegPhase(CallMediaLegId id) const;
  CallMediaSessionPhase Phase() const;

  Roe<void> SendMedia(CallMediaLegId id, uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                      uint8_t mark = 0);
  Roe<void> SendAudio(CallMediaLegId id, const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  amp::MeshRuntime& runtime_;
};

} // namespace pbr
