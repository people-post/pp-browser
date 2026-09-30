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
  /** A candidate path is being brought up (migration in flight). */
  bool candidate = false;
  /** k4: a warm fallback path the call can fail over to. */
  bool standby = false;
  CallMediaLinkKind standby_kind = CallMediaLinkKind::Unknown;
  /** k4: the call lost its last path and waits (reconnect window) for a new one. */
  bool reconnecting = false;
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
  std::string ActiveRemotePeerId() const;
  /** Any thread: the primary bundle's active link, read under the link strand (not under this lock). */
  CallLinkCounters ActiveLinkCounters() const;
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
  /**
   * k6 (K003): bring the peer's Connected link of `kind` up as the call's warm standby — the same
   * handshake as a migration (`path_add`), but TX stays on the active path. Refused when the call
   * already has a standby; a peer without k6 rejects it. `done` on the IO strand.
   */
  void AddStandbyLegOfKind(CallMediaLegId id, CallMediaLinkKind kind, LegFinished done);
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
  /** Test: send nothing (media, heartbeats) on paths of `kind` — the path goes quiet, its link stays up. */
  void SetSilencedPathKindForTest(CallMediaLinkKind kind);
  void SetReconnectWindowForTest(std::chrono::milliseconds window);

  Roe<void> SendMedia(CallMediaLegId id, uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                      uint8_t mark = 0);
  Roe<void> SendAudio(CallMediaLegId id, const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
};

} // namespace pbr
