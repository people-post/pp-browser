#pragma once

#include "domain/media/CallMediaEngine.h"
#include "feature/calls/SharedPorts.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "domain/messaging/SoftMigrateLogic.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/calls/CallHopDriver.h"
#include "feature/calls/CallHopPlanning.h"
#include "feature/calls/CallHopRanking.h"
#include "feature/calls/CallTopologyHostPorts.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallHopMigrateWorkflow.h"
#include "domain/messaging/CallHopPlannerLogic.h"
#include "foundation/runtime/DeferredSelf.h"

#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Hop arming / progress — Topology consumer contract (V048).
 * Empty ports = permissive (unit tests). Owner projects into owned Workflow migrate ports.
 */
struct CallHopArmingPorts {
  std::function<bool()> hop_ops_allowed;
  std::function<bool()> soft_migrate_may_arm;
  std::function<uint64_t()> media_cancel_gen;
  std::function<void(CallHopPlannerPhase phase, const std::string& call_id)> report_progress;
  std::function<const char*()> arming_debug_name;

  bool IsBound() const { return static_cast<bool>(hop_ops_allowed); }
};

/**
 * MediaSeat façade for Topology (V046) — fuller than CSM CallMediaSeatPorts /
 * Workflow CallHopMigrateSeatPorts. Topology must not hold CallMediaSeat*.
 */
struct CallTopologySeatPorts {
  std::function<bool(const std::string& call_id)> is_bound;
  std::function<std::string()> bound_call_id;
  std::function<CallMediaSeat::Token(const std::string& call_id)> acquire;
  std::function<bool(const CallMediaSeat::Token& token)> allows_path_op;
  std::function<CallMediaSeat::AttachBeginResult(const std::string& call_id, const std::string& hop,
                                                 CallMediaSeat::AttachTicket* ticket)>
      begin_attach;
  std::function<void(const std::string& call_id, const std::string& hop)> end_attach_if_matching;
  std::function<bool()> has_attach_in_flight;
  std::function<std::string()> attaching_hop;
  std::function<void(const std::string& call_id)> note_connecting;
  std::function<void(const std::string& call_id)> note_start;
  std::function<void(CallMediaSeat::PathKind kind)> note_path;
  std::function<void(const std::string& call_id)> note_live;
  std::function<void(const std::string& call_id)> cancel_attach_for_call;

  bool IsBound() const { return static_cast<bool>(is_bound); }
};

/**
 * SFU soft-migrate / attach-wait / hop pick (V021 + V025) — V036 Phase 3 **Hop path** plugin
 * under CallMediaSeat. Pure who-picks / wait / fan-out live in base SoftMigrateLogic /
 * CallHopAttachLogic; this adapter owns IO + AppRuntime posting.
 * Attach StartSfu requires a seat token when the seat is wired.
 *
 * SoftMigrate race clusters live on CallHopMigrateWorkflow (V047); this type keeps refs +
 * Host/HopArming/Seat ports for Topology-local control paths.
 */
class CallTopologyController : public Module, public CallHopDriver {
public:
  using HostPorts = CallTopologyHostPorts;
  using MediaRelayDeps = CallTopologyMediaRelayDeps;

  CallTopologyController(CallSessionStore& sessions, ContactsStore& contacts, CallMediaEngine& media);
  ~CallTopologyController() override;
  CallTopologyController(const CallTopologyController&) = delete;
  CallTopologyController& operator=(const CallTopologyController&) = delete;

  void SetHostPorts(HostPorts ports);
  /** Swapping deps moves the relay session-end observer to the new relay (empty = unwatch). */
  void SetMediaRelayDeps(MediaRelayDeps deps);
  /** Required for SFU E2E AEAD (V032). */
  void SetMediaKeyStore(CallMediaKeyStore* keys);
  /** V046 exclusive media bind / epoch via ports. */
  void SetSeatPorts(CallTopologySeatPorts ports);
  /** V048 hop arming / progress — empty ports = permissive (unit tests). */
  void SetHopArmingPorts(CallHopArmingPorts ports);

  bool IsAwaitingSfuRecovery() const override;
  bool IsSfuAttached() const override;
  /** Hop driver: SoftMigrate / hint / N≥3 / recovery — the call's 1:1 stream closing is expected. */
  bool ExpectsGroupMedia(const std::string& call_id) const override;
  bool IsOnSfuForCall(const std::string& call_id) const;
  /** Soft-migrate / attach-wait in flight (suppress ICE→SFU re-entry + stale LeaveCall). */
  bool IsSoftMigrateInFlight() const;
  bool IsSfuAttachWaitActive() const;

  /** The media hops this device could use, ranked (shared with SoftMigrate). */
  const CallHopRanking& HopRanking() const { return ranking_; }
  /** V050 group hop planning (plan at StartCall, probe while ringing, reports, join resolution). */
  CallHopPlanning& HopPlanning() { return planning_; }

  void BeginSfuAttachWait(const std::string& call_id) override;
  void ClearSfuAttachWait();
  void PollPendingSfuAttach();

  void OnMediaStopped(const std::string& call_id) override;

  void EjectParticipantAfterMigrateFailure(const std::string& call_id, const std::string& identity,
                                           const std::string& reason);

  /** SoftMigrate with quote / attach as completions (no thread parks on them). */
  void MaybeSoftMigrateToSfuAsync(const std::string& call_id, SoftMigrateTrigger trigger,
                                  const std::string& prefer_hop_peer_id, uint64_t expected_gen,
                                  std::function<void(Roe<void>)> on_done);
  void AttachLocalToSfuAsync(const std::string& call_id, const CallSfuAttachDetail& attach,
                             std::function<void(Roe<void>)> on_done);


  /**
   * After local AcceptInvite: attach via hint, soft-migrate, or clear wait for 1:1.
   * Returns true if an SFU path was scheduled (caller should not start P2P).
   */
  bool OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                           const std::optional<std::string>& sfu_hint) override;

  /**
   * After inbound CallAccept raised joined count: soft-migrate or clear SFU wait.
   * Returns true if SFU path was taken (caller should not start P2P offerer).
   */
  bool OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                            const std::string& joiner_identity) override;

  /**
   * After CallRoster updated local joined count (mid-call invite path): initiator may SoftMigrate.
   */
  void OnJoinedCountObserved(const std::string& call_id, size_t n_joined);

  /**
   * Peer learned media_relay=true (caps). SoftMigrate re-pick for N≥3 / attach-wait only
   * (V038 — never nudge plain 1:1 onto PreferLocal SoftMigrate).
   */
  void OnPeerMediaRelayCapLearned(const std::string& call_id, const std::string& peer_id);

  /**
   * `sender` fanned the attach out. Attached to another hop, a guest follows only the hop owner's
   * attach (V050: the owner moved the group); others' attaches there are publisher announces.
   */
  Roe<void> OnInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach,
                               const std::string& sender = {});
  /** Guest attach failed with hop preferences (V029) — initiator only. */
  void OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail);
  /** Owner refused guest after empty hop intersection (V029). */
  void OnInboundHopRefuse(const CallHopRefuseDetail& detail);

  void RefreshAdaptation(const std::string& call_id);
  void RefreshAdaptation(const std::string& call_id, bool camera_user_wants) override;
  /** Re-Subscribe hop streams for all currently Joined peers (late join / roster). */
  void SyncSfuSubscriptions(const std::string& call_id);
  uint32_t PublisherStreamIdForLocal() const;
  /** Hop health when SFU attached (V032). */
  CallHopHealth HopHealth() const;

  /** V039 Hop planner Apply. */
  void Apply(CallHopPlannerEvent ev, const std::string& call_id = {});
  CallHopPlannerPhase HopPlannerPhase() const;

private:
  void BindHopMigratePortsAndOps();
  CallHopMigrateHostPorts MakeMigrateHostPorts(const HostPorts& ports) const;
  CallHopMigrateArmingPorts MakeMigrateArmingPorts(const CallHopArmingPorts& ports) const;
  CallHopMigrateSeatPorts MakeMigrateSeatPorts(const CallTopologySeatPorts& ports) const;
  void ReportSfuAttachFailedToInitiator(const std::string& call_id, const std::string& failed_hop,
                                        const std::string& error);
  void RefuseGuestNoSharedHop(const std::string& call_id, const std::string& guest_identity);
  std::vector<std::string> DialableHopPeerIds() const;
  bool IsMigrateGenerationCurrent(uint64_t gen) const;
  /** Local advertise MA + InferCallHopScope for SoftMigrate (V035). */
  CallHopScope InferScopeForCall(const std::string& call_id, const std::string& local_identity) const;
  bool LanReachabilityConfirmedForCall(const std::string& call_id,
                                       const std::string& local_identity) const;
  /** Joined remotes of the call (local excluded) — the peers SoftMigrate scope / LAN checks cover. */
  std::vector<std::string> JoinedRemoteIdentities(const std::string& call_id, const std::string& local_identity) const;
  bool IsActiveCallForTopology(const std::string& call_id) const;
  void FanOutSfuAttachForHop(const std::string& call_id, const std::string& hop_peer_id,
                             const std::string& local_identity);
  void FlushPendingHopPrefer(const std::string& call_id);
  /** Apply deferred CallSfuAttach after SoftMigrate finishes (calls owner). */
  void FlushPendingInboundSfuAttach();
  void SubscribePublisherStream(uint32_t stream_id);
  void SetHopPlannerPhase(CallHopPlannerPhase next, CallHopPlannerEvent ev, const std::string& call_id);
  void ReportHopProgress(CallHopPlannerPhase phase, const std::string& call_id);
  CallHopPlannerApplyContext BuildHopPlannerContext(const std::string& call_id, size_t joined_count,
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
  /** Bump the migrate generation and own the flight for `call_id`; returns the new generation. */
  uint64_t ClaimMigrateFlight(const std::string& call_id);
  void ReleaseMigrateFlight();
  bool MigrateFlightBusyFor(const std::string& call_id) const;
  // Local accept steps (OnLocalAcceptJoined → invite hint / group without hint / stay direct).
  void AttachFromInviteHint(const std::string& call_id, const std::string& hop_peer_id);
  void FinishInviteHintAttach(const std::string& call_id, uint64_t gen, const Roe<void>& ok);
  void JoinGroupWithoutHint(const std::string& call_id, size_t n_joined);
  void FinishJoinSoftMigrate(const std::string& call_id, uint64_t gen, const Roe<void>& mig, bool attached_when_done);
  void StayDirectAfterAccept(const std::string& call_id, size_t n_joined);
  // Remote accept steps.
  void RefanOutLocalHopForJoiner(const std::string& call_id, const std::string& joiner_identity);
  void FinishRemoteAcceptMigrate(const std::string& call_id, const std::string& joiner_identity, uint64_t gen,
                                 const Roe<void>& mig);
  // Hop hint (guest could not reach our hop) steps.
  bool IsStickyInitiator(const std::string& call_id, const std::string& local_identity) const;
  /** V050: attached to another hop and the owner fanned out a new one — let go of ours. True = detached. */
  bool LeaveHopForOwnerMove(const std::string& call_id, const CallSfuAttachDetail& attach, const std::string& sender);
  bool HopHintMayLeavePreferLocal(const std::string& prefer_hop_peer_id) const;
  bool IsOnOrHintedHop(const std::string& call_id, const std::string& hop_peer_id) const;
  void StartHopHintRepick(const std::string& call_id, const std::string& prefer, const std::string& guest);
  void FinishHopHintRepick(const std::string& call_id, const std::string& guest, uint64_t gen, const Roe<void>& mig);
  // Inbound CallSfuAttach steps (OnInboundSfuAttach → expect → settle without dial → start → finish on the calls owner).
  bool ExpectsInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach) const;
  void DeferInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach);
  /** True when the attach is settled without dialing (already attached, coalesced, deferred, refused). */
  bool SettleInboundSfuAttachWithoutDial(const std::string& call_id, const CallSfuAttachDetail& attach);
  bool RefusePrivateHopMultiaddr(const std::string& call_id, const CallSfuAttachDetail& attach);
  void StartInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach);
  void FinishInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach, uint64_t gen,
                              const Roe<void>& ok);
  void FinishSupersededInboundSfuAttach(const std::string& call_id, uint64_t gen, const Roe<void>& ok);

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
  SharedPorts<CallTopologySeatPorts> seat_;
  SharedPorts<CallHopArmingPorts> arming_;
  MediaRelayDeps relay_deps_;
  /** Read relay_deps_ (non-owning): the hop ranking and V050 planning. */
  CallHopRanking ranking_;
  CallHopPlanning planning_;
  // Relay session-end observer on relay_deps_.relay; the token drops notices queued before an
  // unwatch or our destruction.
  DeferredSelf relay_loss_self_;
  uint64_t relay_loss_observer_ = 0;

  /** Coordinator timers (attach-wait deadline, publisher re-announce) drop once we are gone. */
  DeferredSelf timers_self_;
  /**
   * V050: at the first group migrate — keep the planned hop, make the one adjustment (replaces the
   * session's planned hop), or refuse the joiner. False = joiner refused (no migration now).
   */
  bool ResolveGroupHopForJoin(const std::string& call_id, const std::string& joiner_identity);
  /** Joined remotes of the call other than `guest` (the members a hop change must suit). */
  std::vector<std::string> JoinedMembersOtherThan(const std::string& call_id, const std::string& guest) const;
  /** V050: per call, guest → the hop the group moved to for it (one change per joiner). */
  std::unordered_map<std::string, std::map<std::string, std::string>> hop_hint_repicked_;
  /** V050: a joined remote this device invited whose CallAccept has not arrived yet. */
  bool AwaitsOwnInviteeAccept(const std::string& call_id, const std::string& local_identity) const;

  void WatchRelayLoss();
  void UnwatchRelayLoss();
};

} // namespace pbr
