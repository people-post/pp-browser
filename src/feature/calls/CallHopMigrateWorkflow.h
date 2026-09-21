#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/CallHopPlannerLogic.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/SoftMigrateLogic.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallTopologyRelayDeps.h"

#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
  std::function<void(const std::string& call_id)> bind_media_call_id;
  std::function<void()> clear_media_peer_identity;
  std::function<void()> release_direct_media;
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
  void ReleaseDirectMedia() const {
    if (release_direct_media) {
      release_direct_media();
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
  };

  struct InboundAttachGate {
    std::mutex mu;
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

  struct PublisherStreams {
    uint32_t local_stream_id = 0;
    std::unordered_set<uint32_t> remote_stream_ids;
    std::unordered_set<uint32_t> video_refresh_sent;
  };

  struct SfuSurface {
    bool attached = false;
    bool awaiting_recovery = false;
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

  Roe<void> MaybeSoftMigrateToSfu(const std::string& call_id, SoftMigrateTrigger trigger,
                                  const std::string& prefer_hop_peer_id = {},
                                  uint64_t expected_gen = 0);
  void MaybeSoftMigrateToSfuAsync(const std::string& call_id, SoftMigrateTrigger trigger,
                                  const std::string& prefer_hop_peer_id, uint64_t expected_gen,
                                  std::function<void(Roe<void>)> on_done);

  Roe<void> CompleteAttachLocalToSfu(const std::string& call_id, CallSfuAttachDetail attach, bool self_hop,
                                     int64_t a_up_bps, uint64_t gen_at_start, uint64_t cancel_gen_at_start,
                                     const std::shared_ptr<std::atomic<bool>>& sfu_frames_ready,
                                     const std::vector<uint8_t>& media_key, uint32_t media_epoch);

  Roe<void> AttachLocalToSfu(const std::string& call_id, const CallSfuAttachDetail& attach);
  void AttachLocalToSfuAsync(const std::string& call_id, const CallSfuAttachDetail& attach,
                             std::function<void(Roe<void>)> on_done);

  void OnGuestSfuTransportLost();
  Roe<void> ReattachGuestSfuTransport(const std::string& call_id, const CallSfuAttachDetail& attach);
  void ReattachGuestSfuTransportAsync(const std::string& call_id, const CallSfuAttachDetail& attach,
                                      std::function<void(Roe<void>)> on_done);

  static constexpr int kMaxGuestSfuReattachAttempts = 3;

private:
  bool IsMigrateGenerationCurrent(uint64_t gen) const;

  CallSessionStore& sessions_;
  CallMediaEngine& media_;
  CallMediaKeyStore* media_keys_ = nullptr;
  CallTopologyMediaRelayDeps* relay_deps_ = nullptr;
  CallHopMigrateHostPorts host_;
  CallHopMigrateArmingPorts arming_;
  CallHopMigrateSeatPorts seat_;
  TopologyOps ops_;
  SoftMigrateFlight flight_;
  AttachWait attach_wait_;
  InboundAttachGate inbound_gate_;
  GuestSfuSession guest_;
  PublisherStreams publishers_;
  SfuSurface sfu_;
};

} // namespace pbr
