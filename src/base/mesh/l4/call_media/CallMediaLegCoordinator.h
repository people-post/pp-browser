#pragma once

#include "amp/link/MeshRuntime.h"
#include "base/mesh/l4/call_media/CallMediaBundleLogic.h"
#include "base/mesh/l4/call_media/ICallMediaTransport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Non-blocking `/pp-browser/call-media/1.0.0` over AMP channel bundles on MeshRuntime.
 * One call attempt = call_id-keyed control+media bundle ([A021]).
 * Product uses CallMediaAmpTransport as the single entry when MeshHost Amp is up ([A020]).
 */
class CallMediaLegCoordinator {
public:
  using WorkerPost = std::function<void(std::function<void()>)>;
  using LegFinished = std::function<void(Roe<void> result)>;
  using InboundHandler = std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)>;

  CallMediaLegCoordinator(pp::amp::MeshRuntime& runtime, WorkerPost post_worker = {});
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
  /** Detach primary/active bundle (empty id → PrimaryBundle). */
  void Detach() { DetachLeg({}); }

  bool IsLegActive(CallMediaLegId id) const;
  bool IsActive() const;
  CallMediaLegId PrimaryLegId() const;
  CallMediaDirectConnectParams ActiveParams() const;
  CallMediaLegPhase LegPhase(CallMediaLegId id) const;
  /** Transitional: maps active bundle phase → CallMediaSessionPhase for existing tests. */
  CallMediaSessionPhase Phase() const;
  CallMediaBundlePhase BundlePhase(CallMediaLegId id) const;

  Roe<void> SendMedia(CallMediaLegId id, uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                      uint8_t mark = 0);
  Roe<void> SendAudio(CallMediaLegId id, const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
};

} // namespace pbr
