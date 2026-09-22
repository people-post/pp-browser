#pragma once

#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/InitiationBillingStore.h"
#include "foundation/data/PricingTypes.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadRecordTypes.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/people/IdentityStore.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Durable session / roster / invite-leave workflow (V044).
 * Owns store mutations + CallSessionLogic transitions. Side effects via HostPorts from
 * CallSessionManager — not a second chrome SM. Nested clusters speak session needs (V048).
 */
class CallSessionWorkflow : public Module {
public:
  /** Delivery / roster wire + chrome activity banners. */
  struct WirePorts {
    std::function<Roe<std::string>()> local_relay_identity;
    std::function<void()> notify_ring_changed;
    std::function<Roe<void>(const std::string& peer, CallControlType type, const std::string& detail,
                            const std::string& display)>
        send_direct;
    /** Pre-mint e2e_public control DM so SoftMigrate/roster fan-out does not race catalog create. */
    std::function<Roe<std::string>(const std::string& peer)> ensure_control_thread;
    std::function<Roe<void>(const std::string& call_id, CallControlType type, const std::string& detail,
                            const std::string& display, const std::string& skip)>
        fan_out_joined;
    std::function<Roe<void>(const std::string& call_id, CallControlType type, const std::string& detail,
                            const std::string& display, const std::string& skip)>
        fan_out_joined_and_ringing;
    std::function<Roe<void>(const std::string& thread_id, CallControlType type, const std::string& text,
                            const std::string& detail)>
        append_origin_history;
    std::function<Roe<CallRosterDetail>(const std::string& call_id)> build_roster_detail;
    std::function<void()> sync_inbox_from_wake;
    std::function<void()> clear_media_activity;

    bool IsBound() const { return static_cast<bool>(local_relay_identity); }
  };

  /** Duplex start/stop + engine queries the session workflow needs. */
  struct DuplexPorts {
    std::function<void(const std::string& call_id)> stop_media_if_call;
    std::function<void(const std::string& call_id, const std::string& peer, bool offerer)>
        schedule_start_direct;
    std::function<void(const std::string& call_id)> on_media_key_ready;
    std::function<bool()> media_is_active;
    std::function<bool()> media_is_sfu_mode;
    std::function<std::string()> media_active_call_id;
    std::function<void()> media_request_keyframe;
    std::function<void()> media_stop;
  };

  /** Hop-path / SFU attach outcomes observed by durable session. */
  struct HopPathPorts {
    std::function<bool(const std::string& call_id, size_t planner_n,
                       const std::optional<std::string>& sfu_hint)>
        on_local_accept_joined;
    std::function<bool(const std::string& call_id, size_t n_joined, const std::string& peer)>
        on_remote_accept_joined;
    std::function<void(const std::string& call_id, size_t n_joined)> on_joined_count_observed;
    std::function<void()> clear_sfu_attach_wait;
    std::function<Roe<void>(const std::string& call_id, const CallSfuAttachDetail&)> on_inbound_sfu_attach;
    std::function<void(const CallSfuAttachFailedDetail&)> on_inbound_sfu_attach_failed;
    std::function<void(const CallHopRefuseDetail&)> on_inbound_hop_refuse;
    std::function<bool(const std::string& call_id)> is_on_sfu_for_call;
    std::function<bool()> has_media_relay_hop_candidates;
  };

  /** Session chrome / arming observations (projected from Lifecycle by CSM). */
  struct ChromePorts {
    std::function<void(const std::string& call_id)> note_direct_connecting;
    /** Arm OutboundCalling/Deciding as soon as call_id exists — before Invite hits the wire. */
    std::function<void(const std::string& call_id)> note_outbound_started;
    std::function<std::string()> accepting_call_id;
    std::function<std::string()> active_call_id;
    std::function<void(const std::string& call_id)> apply_remote_ended;
    std::function<bool()> is_outbound_calling;
  };

  /** Peer reach, caps, listen addrs, media-key send. */
  struct ReachPorts {
    std::function<void(const std::string& identity, const std::vector<std::string>&)> register_peer_listen;
    std::function<void(const std::string& identity, const CallPeerCaps& caps,
                       const std::vector<std::string>& listen)>
        note_caps_for_identity;
    std::function<void(const std::string& identity)> prefetch_reach;
    /** Kick mesh circuit park (composition projects CallMediaPlane::EnsureCircuitReady). */
    std::function<void()> ensure_circuit_ready;
    /** Block until circuit-ready (Accept gate). Returns true if ready. */
    std::function<bool(int timeout_ms)> await_circuit_ready;
    std::function<void(const std::string& relay, const std::string& peer_id)> note_mesh_peer_id_for_relay;
    std::function<Roe<ByteVector>(const std::string& peer)> resolve_peer_session_key;
    std::function<Roe<void>(const std::string& call_id, const std::string& peer, uint32_t epoch,
                            const std::string& key_id, const ByteVector& key)>
        send_media_key;
    std::function<std::vector<std::string>()> local_listen_multiaddrs;
    std::function<CallPeerCaps()> local_peer_caps;
    std::function<std::string()> local_mesh_peer_id;
  };

  struct HostPorts {
    WirePorts wire;
    DuplexPorts duplex;
    HopPathPorts hop;
    ChromePorts chrome;
    ReachPorts reach;

    bool IsBound() const { return wire.IsBound(); }
  };

  CallSessionWorkflow(IThreadStore& store, IdentityStore& identity, CallSessionStore& sessions,
                      CallMediaKeyStore& media_keys);

  void SetHostPorts(HostPorts ports);
  void SetInitiationBillingStore(InitiationBillingStore* store) { initiation_billing_ = store; }
  InitiationBillingStore* InitiationBilling() const { return initiation_billing_; }

  Roe<CallSession> StartCall(const std::string& origin_thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);
  Roe<void> AcceptInvite(const std::string& call_id,
                         InitiationChargeDecision charge_decision = InitiationChargeDecision::Waive);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id);
  Roe<void> LeaveCallIfActiveExcept(const std::string& keep_call_id);
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms);
  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);

  void SweepExpiredInvites();
  void AbandonOrphanedCallsAfterRestart();

  int64_t InitiationOfferMinorForPeer(const std::string& peer_identity) const;
  void SetPendingAcceptChargeDecision(InitiationChargeDecision decision);

  /** Peer remembered for Lifecycle KickAnswerer after Accept (peek; cleared on Leave). */
  void ClearPendingAnswererKick();
  bool PeekPendingAnswererKick(const std::string& call_id, std::string* peer_out) const;

  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();

  Roe<void> HandleInboundInvite(const std::string& detail_json, const std::string& sender_identity,
                                const ThreadMessage& message, std::optional<int64_t> relay_created_at_ms,
                                std::optional<int64_t> relay_server_time_ms, const std::string& local_identity);
  Roe<void> HandleInboundAccept(const std::string& detail_json, const std::string& sender_identity,
                                const std::string& local_identity);
  Roe<void> HandleInboundDecline(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundLeave(const std::string& detail_json, const std::string& sender_identity,
                               const std::string& local_identity);
  Roe<void> HandleInboundRoster(const std::string& detail_json);
  Roe<void> HandleInboundMediaKey(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundSfuAttach(const std::string& detail_json);
  Roe<void> HandleInboundSfuAttachFailed(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundHopRefuse(const std::string& detail_json);
  Roe<void> HandleInboundVideoRefresh(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundEnded(const std::string& detail_json, const std::string& local_identity);

private:
  IThreadStore& store_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  HostPorts host_;
  InitiationBillingStore* initiation_billing_ = nullptr;
  InitiationChargeDecision pending_accept_charge_ = InitiationChargeDecision::Waive;
  bool pending_accept_charge_set_ = false;
  std::string pending_answerer_kick_call_id_;
  std::string pending_answerer_kick_peer_;
};

} // namespace pbr
