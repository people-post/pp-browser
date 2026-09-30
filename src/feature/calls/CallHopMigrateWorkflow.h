#pragma once

#include "domain/media/CallMediaEngine.h"
#include "feature/calls/CallMediaCoordinator.h"
#include "feature/calls/SharedPorts.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/CallHopPlannerLogic.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/SoftMigrateLogic.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "feature/calls/CallMediaSeat.h"
#include "domain/mesh/media_plane/MediaRelayAttach.h"
#include "feature/calls/CallSessionEvents.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "foundation/runtime/OwnerOutbox.h"
#include "foundation/runtime/OwnerSteps.h"
#include "foundation/runtime/DeferredSelf.h"

#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * SoftMigrate / attach host side effects — CallHopMigrateWorkflow consumer contract (V048).
 * Subset of Topology host surface; Topology (owner) projects. Empty = no-op helpers.
 */
struct CallHopMigrateHostPorts {
  std::function<Roe<std::string>()> local_relay_identity;
  std::function<Roe<void>(const std::string& call_id, CallControlType type, const std::string& detail_json,
                          const std::string& display, const std::string& skip_identity)>
      fan_out_joined;
  std::function<void()> notify_ring_changed;
  std::function<void(std::string message)> set_last_media_error;
  std::function<void(std::string message)> set_media_activity;
  std::function<void()> clear_media_activity;
  std::function<void(const std::string& call_id)> note_media_attempted;
  /** The call's media coordinator: the hop path starts / stops the engine through it. */
  std::function<CallMediaCoordinator*(const std::string& call_id)> call_media;
  std::function<void()> clear_media_peer_identity;
  std::function<void()> request_inbox_sync;

  bool IsBound() const { return static_cast<bool>(local_relay_identity); }

  void ClearMediaActivity() const {
    if (clear_media_activity) {
      clear_media_activity();
    }
  }
  void NotifyRingChanged() const {
    if (notify_ring_changed) {
      notify_ring_changed();
    }
  }
  void ClearMediaPeerIdentity() const {
    if (clear_media_peer_identity) {
      clear_media_peer_identity();
    }
  }
  void RequestInboxSync() const {
    if (request_inbox_sync) {
      request_inbox_sync();
    }
  }
  void SetMediaActivity(std::string message) const {
    if (set_media_activity) {
      set_media_activity(std::move(message));
    }
  }
  void SetLastMediaError(std::string message) const {
    if (set_last_media_error) {
      set_last_media_error(std::move(message));
    }
  }
};

/**
 * SoftMigrate / attach arming — CallHopMigrateWorkflow consumer contract (V048).
 * Empty ports = permissive (unit tests). Topology (owner) projects hop arming into these.
 */
struct CallHopMigrateArmingPorts {
  std::function<bool()> migrate_ops_allowed;
  std::function<bool()> soft_migrate_may_arm;
  std::function<uint64_t()> media_cancel_gen;
  std::function<void(CallHopPlannerPhase phase, const std::string& call_id)> report_progress;
  std::function<const char*()> arming_debug_name;

  bool IsBound() const { return static_cast<bool>(migrate_ops_allowed); }
};

/**
 * MediaSeat façade for SoftMigrate / Attach (V047/V048).
 * Subset of Topology's seat surface — only ops the migrate executor calls.
 */
struct CallHopMigrateSeatPorts {
  std::function<bool(const std::string& call_id)> is_bound;
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

  bool IsBound() const { return static_cast<bool>(is_bound); }
};

/**
 * SoftMigrate + SFU attach / guest reattach (V046/V047).
 * Owns race-state clusters; side effects via Host/MigrateArming/MigrateSeat ports + TopologyOps.
 * No friend access into CallTopologyController; does not include Topology vocabulary.
 */
class CallHopMigrateWorkflow : public Module {
public:
  /** SoftMigrate in-flight / hop pick race state. */
  struct SoftMigrateFlight {
    bool in_flight = false;
    uint64_t flight_gen = 0;
    std::string call_id;
    std::atomic<uint64_t> migrate_generation{0};
    std::string attaching_hop_peer_id;
    std::string attached_hop_peer_id;
    std::string pending_hop_prefer;
  };

  struct AttachWait {
    std::string call_id;
    int64_t deadline_ms = 0;
    uint64_t timer_id = 0;
    /** Bumped at each arm: a deadline event for an earlier arm is stale. */
    uint64_t armed = 0;
  };

  /** Deferred inbound CallSfuAttach + last failure. Calls owner only (hop migrate and topology). */
  struct InboundAttachGate {
    std::optional<CallSfuAttachDetail> pending_attach;
    std::string pending_call_id;
    std::string last_fail_call_id;
    std::string last_fail_hop;
  };

  struct GuestSfuSession {
    std::optional<CallSfuAttachDetail> active_attach;
    std::string active_call_id;
    int reattach_attempts = 0;
    bool reattach_in_flight = false;
  };

  /**
   * Written on the calls owner and read off it (relay subscription paths), so the sets are
   * guarded by `mu` and the stream id is atomic. Do relay I/O outside `mu`.
   */
  struct PublisherStreams {
    std::atomic<uint32_t> local_stream_id{0};
    std::mutex mu;
    std::unordered_set<uint32_t> remote_stream_ids;
    std::unordered_set<uint32_t> video_refresh_sent;
  };

  struct SfuSurface {
    /** Owner writes; the 1:1 receive path reads it on transport I/O (the hop carries the call). */
    std::atomic<bool> attached{false};
    int64_t last_quote_a_up_bps = 0;
    CallHopPlannerPhase hop_planner_phase = CallHopPlannerPhase::Idle;
  };

  /** Topology helpers that remain on CallTopologyController (ranking / fan-out / planner Apply). */
  struct TopologyOps {
    std::function<void(CallHopPlannerEvent ev, const std::string& call_id)> apply;
    std::function<void(const std::string& call_id)> sync_sfu_subscriptions;
    std::function<void(const std::string& call_id)> refresh_adaptation;
    std::function<std::vector<MeshHopCandidate>()> ranked_media_hop_candidates;
    std::function<std::string(const std::string& hop_peer_id)> resolve_hop_multiaddr;
    std::function<std::string(const std::string& local_peer_id)> resolve_local_advertise_ma;
    std::function<CallHopScope(const std::string& call_id, const std::string& local_identity)>
        infer_scope_for_call;
    std::function<bool(const std::string& call_id, const std::string& local_identity)>
        lan_reachability_confirmed_for_call;
    std::function<void(const std::string& call_id, const std::string& hop_peer_id,
                       const std::string& local_identity)>
        fan_out_sfu_attach_for_hop;
    std::function<uint32_t()> publisher_stream_id_for_local;
    std::function<void(const CallSfuAttachDetail& attach)> note_remote_publisher_from_attach;
    std::function<void(const std::string& call_id, const CallSfuAttachDetail& hop_attach)>
        announce_local_publisher;
    std::function<bool(const std::string& call_id)> is_active_call_for_topology;
    std::function<void(const std::string& call_id)> begin_sfu_attach_wait;
    std::function<void()> clear_sfu_attach_wait;

    bool IsBound() const { return static_cast<bool>(apply); }
  };

  CallHopMigrateWorkflow(CallSessionStore& sessions, CallMediaEngine& media);

  void SetHostPorts(CallHopMigrateHostPorts ports);
  /** Where it reports its delayed follow-ups (its parent binds it). */
  void SetOutbox(OwnerOutbox<HopMigrateEvent> outbox) { outbox_ = std::move(outbox); }
  /** A follow-up it reported, back from the calls owner's queue. */
  void Handle(HopMigrateEvent& event);
  void SetArmingPorts(CallHopMigrateArmingPorts ports);
  void SetSeatPorts(CallHopMigrateSeatPorts ports);
  void SetTopologyOps(TopologyOps ops);
  void SetMediaRelayDeps(CallTopologyMediaRelayDeps* deps);
  void SetMediaKeyStore(CallMediaKeyStore* keys);

  SoftMigrateFlight& Flight() { return flight_; }
  const SoftMigrateFlight& Flight() const { return flight_; }
  AttachWait& AttachWaitState() { return attach_wait_; }
  const AttachWait& AttachWaitState() const { return attach_wait_; }
  InboundAttachGate& InboundGate() { return inbound_gate_; }
  GuestSfuSession& Guest() { return guest_; }
  const GuestSfuSession& Guest() const { return guest_; }
  PublisherStreams& Publishers() { return publishers_; }
  const PublisherStreams& Publishers() const { return publishers_; }
  SfuSurface& Sfu() { return sfu_; }
  const SfuSurface& Sfu() const { return sfu_; }

  void MaybeSoftMigrateToSfuAsync(const std::string& call_id, SoftMigrateTrigger trigger,
                                  const std::string& prefer_hop_peer_id, uint64_t expected_gen,
                                  std::function<void(Roe<void>)> on_done);

  void AttachLocalToSfuAsync(const std::string& call_id, const CallSfuAttachDetail& attach,
                             std::function<void(Roe<void>)> on_done);

  void OnGuestSfuTransportLost();
  void ReattachGuestSfuTransportAsync(const std::string& call_id, const CallSfuAttachDetail& attach,
                                      std::function<void(Roe<void>)> on_done);

  static constexpr int kMaxGuestSfuReattachAttempts = 3;

private:
  struct HopPick;
  struct HopAttach;

  bool IsMigrateGenerationCurrent(uint64_t gen) const;
  bool IsLiveOnHopFor(const std::string& call_id) const;

  // SoftMigrate steps (MaybeSoftMigrateToSfuAsync → gate → control thread → pick hops in order).
  /** False when the arming state settles the request here (`on_done` already called). */
  bool PassSoftMigrateArmingGate(const std::string& call_id, SoftMigrateTrigger trigger,
                                 const std::string& prefer_hop_peer_id, const std::function<void(Roe<void>)>& on_done);
  void RunSoftMigrate(const std::string& call_id, SoftMigrateTrigger trigger, const std::string& prefer_hop_peer_id,
                      uint64_t expected_gen, std::function<void(Roe<void>)> on_done);
  SoftMigrateAction DecideFirstSoftMigrate(const HopPick& pick, SoftMigrateTrigger trigger,
                                           const std::vector<CallParticipant>& participants);
  bool PreferLocalHopAllowed(const std::string& call_id, const std::string& local_identity) const;
  bool HasDurableMediaRelayHop() const;
  /** Re-pick while attached: true when settled on the current hop, false after detaching to re-pick. */
  bool SettleRepickOnCurrentHop(HopPick& pick, const std::string& prefer_hop_peer_id);
  std::vector<MeshHopCandidate> RankHopsForSoftMigrate(HopPick& pick, const std::string& prefer_hop_peer_id);
  void TryPickHop(std::shared_ptr<HopPick> pick, size_t index);
  void FailHopPick(HopPick& pick);
  void AttachPickedHop(std::shared_ptr<HopPick> pick, size_t index);
  void OnPickedHopAttached(const std::shared_ptr<HopPick>& pick, size_t index, bool self_hop,
                           const CallSfuAttachDetail& attach, Roe<void> attached);
  void RecordPickedHop(HopPick& pick, const std::string& hop_peer_id);
  void FanOutPickedHop(const std::string& call_id, const CallSfuAttachDetail& attach,
                       const std::string& local_identity);

  // Hop attach steps (AttachLocalToSfuAsync → claim → key → local hop / relay → CompleteHopAttach on the calls owner).
  /** True = this attempt owns the attach; false = coalesced into another (done, not an error). */
  Roe<bool> ClaimHopAttachFlight(const std::string& call_id, const CallSfuAttachDetail& attach);
  std::function<void(Roe<void>)> ReleaseHopAttachFlightOnError(const std::string& call_id, const std::string& hop,
                                                               std::function<void(Roe<void>)> on_done);
  Roe<void> LoadHopMediaKey(HopAttach& at) const;
  std::function<void(MediaDataFrame)> MakeHopFrameSink(const HopAttach& at);
  void AttachAsLocalHop(HopAttach at, std::function<void(Roe<void>)> on_done);
  void AttachThroughRelay(HopAttach at, std::function<void(Roe<void>)> on_done);
  /** Calls owner: commit an attached hop (StartSfu, state, chrome) unless the call moved on. */
  Roe<void> CompleteHopAttach(const HopAttach& at, int64_t a_up_bps);
  void ApplyQuoteAdaptation(int64_t a_up_bps);
  Roe<void> CheckHopAttachStillWanted(const HopAttach& at);
  bool OwnsHopAttachFlight(const HopAttach& at) const;
  /** Detach the relay session and report the attach as aborted. */
  Roe<void> AbortHopAttach();
  CallMediaEngine::SfuSendFn MakeHopSendFn(const HopAttach& at);
  Roe<void> StartHopMedia(const HopAttach& at);
  void MarkHopAttachLive(const HopAttach& at, bool fresh_start);
  void ReleaseDirectAfterHopAttach(const HopAttach& at);
  void RefanOutPickedHop(const hop_migrate_event::RefanOutPickedHop& again);
  /** Run `step` as the next event (never inside the caller). */
  void Defer(std::function<void()> step);
  uint64_t StoreAttach(HopAttach at, std::function<void(Roe<void>)> on_done, bool guest);
  std::function<void(Roe<MediaRelayAttached>)> RelayAttachReporter(uint64_t id) const;
  void OnRelayAttached(const hop_migrate_event::RelayAttached& answer);
  void ReleaseDirectSettled(const hop_migrate_event::ReleaseDirectAfterAttach& release);
  void ReleaseDirectFor(const std::string& call_id);
  // Guest reattach after a lost relay transport (engine stays live).
  void StartGuestReattach(const std::string& call_id, const CallSfuAttachDetail& attach_in,
                          std::function<void(Roe<void>)> on_done);
  Roe<void> CompleteGuestReattach(const HopAttach& at, int64_t a_up_bps);
  /** media_relay attach mechanism (domain/mesh MediaRelayAttach) over this workflow's relay deps. */
  MediaRelayAttachPorts RelayAttachPorts() const;
  /** Call policy for a relay attach: session id / auth = call id; quote sized by roster + video. */
  MediaRelayAttachRequest MakeRelayAttachRequest(const std::string& call_id, const CallSfuAttachDetail& attach) const;
  static std::function<Roe<void>(const MediaRelayQuote&)> RelayQuotePricingGate();

  CallSessionStore& sessions_;
  CallMediaEngine& media_;
  CallMediaKeyStore* media_keys_ = nullptr;
  CallTopologyMediaRelayDeps* relay_deps_ = nullptr;
  CallHopMigrateHostPorts host_;
  SharedPorts<CallHopMigrateArmingPorts> arming_;
  SharedPorts<CallHopMigrateSeatPorts> seat_;
  TopologyOps ops_;
  SoftMigrateFlight flight_;
  AttachWait attach_wait_;
  InboundAttachGate inbound_gate_;
  GuestSfuSession guest_;
  PublisherStreams publishers_;
  SfuSurface sfu_;
  /** Coordinator timers (re-fan-out, settle, reattach backoff) drop once we are gone. */
  OwnerOutbox<HopMigrateEvent> outbox_;
  /** Steps waiting for their Continue event (deferred steps, a picked hop's reach answer). */
  OwnerSteps steps_;
  /** Relay attaches waiting for the relay's answer. */
  struct PendingAttach {
    std::shared_ptr<HopAttach> at;  // HopAttach is private to the .cpp
    std::function<void(Roe<void>)> on_done;
    bool guest = false;
  };
  std::unordered_map<uint64_t, PendingAttach> attaches_;
  uint64_t next_attach_ = 0;
};

} // namespace pbr
