#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/l4/call_media/CallMediaBundleLogic.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

/** A call's paths (call-path-resilience k3): the one media flows on, and a migration's others. */
struct CallMediaPathState {
  CallMediaLinkKind active_kind = CallMediaLinkKind::Unknown;
  uint32_t active_gen = 0;
  /** A candidate path is being brought up. */
  bool standby = false;
  /** The previous path is draining after a switch. */
  bool retiring = false;
};

/**
 * Non-blocking `/pp-browser/realtime/1.0.0` over AMP channel bundles on MeshRuntime.
 * One call attempt = call_id-keyed control+media bundle ([A021]).
 * Product uses CallMediaAmpTransport as the single entry when MeshHost Amp is up ([A020]).
 */
class CallMediaLegCoordinator {
public:
  using LegFinished = std::function<void(Roe<void> result)>;
  using InboundHandler = CallMediaInboundHandler;

  explicit CallMediaLegCoordinator(pp::amp::MeshRuntime& runtime);
  ~CallMediaLegCoordinator();

  CallMediaLegCoordinator(const CallMediaLegCoordinator&) = delete;
  CallMediaLegCoordinator& operator=(const CallMediaLegCoordinator&) = delete;

  void Start();
  void Stop();

  /** See ICallMediaTransport::SetInboundHandler — called on IO, answers on any thread. */
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
  CallMediaLinkKind ActiveLinkKind() const;
  CallMediaLegPhase LegPhase(CallMediaLegId id) const;
  /** Transitional: maps active bundle phase → CallMediaSessionPhase for existing tests. */
  CallMediaSessionPhase Phase() const;
  CallMediaBundlePhase BundlePhase(CallMediaLegId id) const;

  /**
   * k3 make-before-break: move leg `id`'s media to `link` (a Connected link to the same peer —
   * e.g. a punched direct link while the call runs on a relay). Either side may start one; if both
   * do at once the glare winner's goes ahead (offerer, then PeerId order). `done` gets OK once media
   * flows on the new path (the old one is then released), or why the call stayed where it was:
   * peer refused / yielded / older peer (timeout), candidate lost. Runs on IO; `done` on the IO strand.
   */
  void MigrateLeg(CallMediaLegId id, pp::amp::LinkHandle link, LegFinished done);
  /** MigrateLeg onto the peer's Connected link of `kind` (direct ADP or relay carrier). */
  void MigrateLegToKind(CallMediaLegId id, CallMediaLinkKind kind, LegFinished done);
  CallMediaPathState PathState(CallMediaLegId id) const;
  /**
   * Default on: when a relayed call's peer becomes reachable over a Connected direct link (a punch
   * landed, or the peer dialed), the driver migrates the call there (`MigrateLeg`), retrying after
   * a 10 s backoff. Off for tests that drive `MigrateLeg` themselves.
   */
  void SetAutoMigrateToDirect(bool enable);
  /** Test: behave like a peer from before k3 (never answers `migrate`). */
  void SetIgnoreMigrateForTest(bool ignore);
  void SetMigrateTimeoutForTest(std::chrono::milliseconds timeout);

  Roe<void> SendMedia(CallMediaLegId id, uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                      uint8_t mark = 0);
  Roe<void> SendAudio(CallMediaLegId id, const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
};

} // namespace pbr
