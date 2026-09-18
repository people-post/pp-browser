#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "domain/messaging/SoftMigrateLogic.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/calls/CallTopologyHostPorts.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "feature/calls/CallTopologyLifecyclePorts.h"
#include "feature/calls/CallTopologySeatPorts.h"
#include "feature/calls/CallHopMigrateWorkflow.h"
#include "domain/messaging/CallHopPlannerLogic.h"

#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * SFU soft-migrate / attach-wait / hop pick (V021 + V025) — V036 Phase 3 **Hop path** plugin
 * under CallMediaSeat. Pure who-picks / wait / fan-out live in base SoftMigrateLogic /
 * SfuAttachWaitLogic / SfuAttachFanout; this adapter owns IO + AppRuntime posting.
 * Attach StartSfu requires a seat token when the seat is wired.
 *
 * SoftMigrate race clusters live on CallHopMigrateWorkflow (V047); this type keeps refs +
 * Host/Lifecycle/Seat ports for Topology-local control paths.
 */
class CallTopologyController : public Module {
public:
  using HostPorts = CallTopologyHostPorts;
  using MediaRelayDeps = CallTopologyMediaRelayDeps;

  CallTopologyController(CallSessionStore& sessions, ContactsStore& contacts, CallMediaEngine& media);

  void SetHostPorts(HostPorts ports);
  void SetMediaRelayDeps(MediaRelayDeps deps);
  /** Required for SFU E2E AEAD (V032). */
  void SetMediaKeyStore(CallMediaKeyStore* keys);
  /** V046 exclusive media bind / epoch via ports. */
  void SetSeatPorts(CallTopologySeatPorts ports);
  /** V046 Status arming — empty ports = permissive (unit tests). */
  void SetLifecyclePorts(CallTopologyLifecyclePorts ports);

  bool IsAwaitingSfuRecovery() const;
  bool IsSfuAttached() const;
  bool IsOnSfuForCall(const std::string& call_id) const;
  /** Soft-migrate / attach-wait in flight (suppress ICE→SFU re-entry + stale LeaveCall). */
  bool IsSoftMigrateInFlight() const;
  bool IsSfuAttachWaitActive() const;

  bool HasMediaRelayHopCandidates() const;
  std::vector<MeshHopCandidate> RankedMediaHopCandidates() const;
  /** Resolve dialable multiaddr for a hop PeerId (contacts ∪ seeds ∪ L1 address book). */
  std::string ResolveHopMultiaddr(const std::string& hop_peer_id) const;

  void BeginSfuAttachWait(const std::string& call_id);
  void ClearSfuAttachWait();
  void PollPendingSfuAttach();

  void ClearAwaitingSfuRecovery();
  void OnMediaStopped(const std::string& call_id);

  void EjectParticipantAfterMigrateFailure(const std::string& call_id, const std::string& identity,
                                           const std::string& reason);

  Roe<void> MaybeSoftMigrateToSfu(const std::string& call_id, SoftMigrateTrigger trigger,
                                  const std::string& prefer_hop_peer_id = {},
                                  uint64_t expected_gen = 0);
  /** SoftMigrate without parking MeshControl on quote/attach (product MeshControl paths). */
  void MaybeSoftMigrateToSfuAsync(const std::string& call_id, SoftMigrateTrigger trigger,
                                  const std::string& prefer_hop_peer_id, uint64_t expected_gen,
                                  std::function<void(Roe<void>)> on_done);
  Roe<void> AttachLocalToSfu(const std::string& call_id, const CallSfuAttachDetail& attach);
  void AttachLocalToSfuAsync(const std::string& call_id, const CallSfuAttachDetail& attach,
                             std::function<void(Roe<void>)> on_done);

  /** Group (N≥3) ICE failed — recover via soft-migrate (posted to UI by caller if needed). */
  void TryRecoverViaSfu(const std::string& call_id);

  /**
   * After local AcceptInvite: attach via hint, soft-migrate, or clear wait for 1:1.
   * Returns true if an SFU path was scheduled (caller should not start P2P).
   */
  bool OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                           const std::optional<std::string>& sfu_hint);

  /**
   * Spine C announce viewer: attach via sfu_hint when present (no SoftMigrate / N≥3 gate).
   * Returns true if an SFU attach was scheduled.
   */
  bool OnAnnounceViewerJoined(const std::string& call_id, const std::optional<std::string>& sfu_hint);

  /**
   * After inbound CallAccept raised joined count: soft-migrate or clear SFU wait.
   * Returns true if SFU path was taken (caller should not start P2P offerer).
   */
  bool OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                            const std::string& joiner_identity);

  /**
   * After CallRoster updated local joined count (mid-call invite path): initiator may SoftMigrate.
   */
  void OnJoinedCountObserved(const std::string& call_id, size_t n_joined);

  /**
   * Peer learned media_relay=true (caps). SoftMigrate re-pick for N≥3 / attach-wait only
   * (V038 — never nudge plain 1:1 onto PreferLocal SoftMigrate).
   */
  void OnPeerMediaRelayCapLearned(const std::string& call_id, const std::string& peer_id);

  Roe<void> OnInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach);
  /** Guest attach failed with hop preferences (V029) — initiator only. */
  void OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail);
  /** Owner refused guest after empty hop intersection (V029). */
  void OnInboundHopRefuse(const CallHopRefuseDetail& detail);

  void RefreshAdaptation(const std::string& call_id);
  void RefreshAdaptation(const std::string& call_id, bool camera_user_wants);
  /** Re-Subscribe hop streams for all currently Joined peers (late join / roster). */
  void SyncSfuSubscriptions(const std::string& call_id);
  uint32_t PublisherStreamIdForLocal() const;
  /** Hop health when SFU attached (V032). */
  CallHopHealth HopHealth() const;

  /** V039 Hop planner Apply. */
  void Apply(CallHopPlannerEvent ev, const std::string& call_id = {});
  CallHopPlannerPhase HopPlannerPhase() const { return sfu_.hop_planner_phase; }

private:
  void BindHopMigratePortsAndOps();
  void ReportSfuAttachFailedToInitiator(const std::string& call_id, const std::string& failed_hop,
                                        const std::string& error);
  void RefuseGuestNoSharedHop(const std::string& call_id, const std::string& guest_identity);
  std::vector<std::string> DialableHopPeerIds() const;
  bool IsMigrateGenerationCurrent(uint64_t gen) const;
  /** Local advertise MA + InferCallHopScope for SoftMigrate (V035). */
  std::string ResolveLocalAdvertiseMa(const std::string& local_peer_id) const;
  std::vector<std::string> ResolveLocalAdvertiseMas() const;
  CallHopScope InferScopeForCall(const std::string& call_id, const std::string& local_identity) const;
  bool LanReachabilityConfirmedForCall(const std::string& call_id,
                                       const std::string& local_identity) const;
  bool IsActiveCallForTopology(const std::string& call_id) const;
  void FanOutSfuAttachForHop(const std::string& call_id, const std::string& hop_peer_id,
                             const std::string& local_identity);
  void FlushPendingHopPrefer(const std::string& call_id);
  /** Apply deferred CallSfuAttach after SoftMigrate finishes (must run on UI). */
  void FlushPendingInboundSfuAttach();
  void SubscribePublisherStream(uint32_t stream_id);
  void SetHopPlannerPhase(CallHopPlannerPhase next, CallHopPlannerEvent ev, const std::string& call_id);
  CallHopPlannerApplyContext BuildHopPlannerContext(const std::string& call_id, size_t effective_n,
                                                    bool has_sfu_hint) const;
  void ArmAttachWaitTimer(const std::string& call_id, int64_t deadline_ms);
  void CancelAttachWaitTimer();
  void OnAttachWaitTimerFire(const std::string& call_id);
  /** First subscribe: ask publisher for an IDR (V034). */
  void MaybeRequestPublisherKeyframe(uint32_t stream_id);
  /** Learn publisher_stream_id from CallSfuAttach even when roster lacks that peer (dogfood). */
  void NoteRemotePublisherFromAttach(const CallSfuAttachDetail& attach);
  /** Fan-out our publisher stream so peers with incomplete Joined roster still subscribe. */
  void AnnounceLocalPublisher(const std::string& call_id, const CallSfuAttachDetail& hop_attach);
  /**
   * Guest media-relay duplex died mid-call — re-AcceptAndAttach without restarting capture.
   * UI-thread entry; work runs on a worker.
   */
  void OnGuestSfuTransportLost();
  /** Quote + AcceptAndAttach + reader + subscribe; keeps existing StartSfu send path. */
  Roe<void> ReattachGuestSfuTransport(const std::string& call_id, const CallSfuAttachDetail& attach);
  void ReattachGuestSfuTransportAsync(const std::string& call_id, const CallSfuAttachDetail& attach,
                                      std::function<void(Roe<void>)> on_done);
  /** StartSfu + fan-out bookkeeping after media-relay attach (or local hop) succeeds. */
  Roe<void> CompleteAttachLocalToSfu(const std::string& call_id, CallSfuAttachDetail attach, bool self_hop,
                                     int64_t a_up_bps, uint64_t gen_at_start, uint64_t cancel_gen_at_start,
                                     const std::shared_ptr<std::atomic<bool>>& sfu_frames_ready,
                                     const std::vector<uint8_t>& media_key, uint32_t media_epoch);

  using SoftMigrateFlight = CallHopMigrateWorkflow::SoftMigrateFlight;
  using AttachWait = CallHopMigrateWorkflow::AttachWait;
  using InboundAttachGate = CallHopMigrateWorkflow::InboundAttachGate;
  using GuestSfuSession = CallHopMigrateWorkflow::GuestSfuSession;
  using PublisherStreams = CallHopMigrateWorkflow::PublisherStreams;
  using SfuSurface = CallHopMigrateWorkflow::SfuSurface;

  CallHopMigrateWorkflow hop_migrate_;
  SoftMigrateFlight& flight_;
  AttachWait& attach_wait_;
  InboundAttachGate& inbound_gate_;
  GuestSfuSession& guest_;
  PublisherStreams& publishers_;
  SfuSurface& sfu_;

  HostPorts host_;
  CallSessionStore& sessions_;
  ContactsStore& contacts_;
  CallMediaEngine& media_;
  CallMediaKeyStore* media_keys_ = nullptr;
  CallTopologySeatPorts seat_;
  CallTopologyLifecyclePorts lifecycle_;
  MediaRelayDeps relay_deps_;
};

} // namespace pbr
