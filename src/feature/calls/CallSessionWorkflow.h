#pragma once

#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallTypes.h"
#include "foundation/runtime/DeferredSelf.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadRecordTypes.h"
#include "domain/messaging/CallSessionStore.h"
#include "feature/calls/CallInitiationBilling.h"
#include "feature/calls/CallMediaKeyExchange.h"
#include "feature/calls/LiveCall.h"

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

  /** The 1:1 start the session workflow schedules. */
  struct DuplexPorts {
    std::function<void(const std::string& call_id, const std::string& peer, bool offerer)>
        schedule_start_direct;
  };

  /** Hop-path / SFU attach outcomes observed by durable session. */
  struct HopPathPorts {
    std::function<void(const std::string& call_id, size_t n_joined)> on_joined_count_observed;
    std::function<void()> clear_sfu_attach_wait;
    /** `sender`: who fanned it out — a hop change is followed only from the hop owner (V050). */
    std::function<Roe<void>(const std::string& call_id, const CallSfuAttachDetail&, const std::string& sender)>
        on_inbound_sfu_attach;
    std::function<void(const CallSfuAttachFailedDetail&)> on_inbound_sfu_attach_failed;
    std::function<void(const CallHopRefuseDetail&)> on_inbound_hop_refuse;
    std::function<bool(const std::string& call_id)> is_on_sfu_for_call;
    std::function<bool()> has_media_relay_hop_candidates;
    /** V050: hop planned from the invite list at StartCall (no attach); nullopt = none. */
    std::function<std::optional<CallPlannedHop>(const std::vector<std::string>& invitees,
                                                const std::string& local_identity)>
        plan_hop_for_invitees;
    /** V050 gt4 invitee: probe the planned hop (+ a few others) while ringing. */
    std::function<void(const std::string& call_id)> probe_invite_hops;
    /** V050 gt4 invitee: the report our CallAccept carries. */
    std::function<CallHopReport(const std::string& call_id)> hop_report_for_accept;
    /** V050 gt4 initiator: a joiner's CallAccept report (before the join decision). */
    std::function<void(const std::string& call_id, const std::string& identity, const CallHopReport& report)>
        note_accept_hop_report;
  };

  /** Peer reach, caps, listen addrs. */
  struct ReachPorts {
    std::function<void(const std::string& identity, const std::vector<std::string>&)> register_peer_listen;
    std::function<void(const std::string& identity, const CallPeerCaps& caps,
                       const std::vector<std::string>& listen)>
        note_caps_for_identity;
    /** The peer's caps for this call (invite / accept) — mobility feeds the call's path policy. */
    std::function<void(const std::string& call_id, const CallPeerCaps& caps)> note_call_peer_caps;
    std::function<void(const std::string& identity)> prefetch_reach;
    /** Kick mesh circuit park (composition projects MeshMediaPlane::ReserveOnBootstrapSeeds). */
    std::function<void()> ensure_circuit_ready;
    /**
     * Await circuit-ready (Accept gate): `done(ready)` runs on the calls owner once parked or at the
     * timeout. Never blocks the caller.
     */
    std::function<void(int timeout_ms, std::function<void(bool ready)> done)> park_circuit;
    std::function<void(const std::string& relay, const std::string& peer_id)> note_mesh_peer_id_for_relay;
    std::function<std::vector<std::string>()> local_listen_multiaddrs;
    std::function<CallPeerCaps()> local_peer_caps;
    std::function<std::string()> local_mesh_peer_id;
  };

  struct HostPorts {
    WirePorts wire;
    DuplexPorts duplex;
    HopPathPorts hop;
    ReachPorts reach;

    bool IsBound() const { return wire.IsBound(); }
  };

  CallSessionWorkflow(IThreadStore& store, CallSessionStore& sessions,
                      CallMediaKeyExchange& key_exchange, CallInitiationBilling& billing, LiveCalls& live_calls);
  ~CallSessionWorkflow() override;

  void SetHostPorts(HostPorts ports);
  /** Bump DeferredSelf so queued Accept/roster PostWorkerNormal cbs no-op (CSM teardown). */
  void InvalidateDeferredOps();

  Roe<CallSession> StartCall(const std::string& origin_thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);
  /**
   * Accept a pending invite: checks → circuit park (async) → CallAccept + Joined + media arm.
   * `on_done` runs once, on the calls owner (inline when there is no park port).
   */
  void AcceptInviteAsync(const std::string& call_id, InitiationChargeDecision charge_decision,
                         std::function<void(Roe<void>)> on_done);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id, LiveCallEndReason reason = LiveCallEndReason::LocalLeave);
  Roe<void> LeaveCallIfActiveExcept(const std::string& keep_call_id);
  /** Every end of a call on this device funnels here; `reason` is what ended it (LiveCall). */
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms, LiveCallEndReason reason);
  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);

  void SweepExpiredInvites();
  void AbandonOrphanedCallsAfterRestart();

  void SetPendingAcceptChargeDecision(InitiationChargeDecision decision);
  /** Set before AcceptClicked — consumed (and reset to false) by AcceptInvite. */
  void SetPendingAcceptVoiceOnly(bool voice_only);

  /** Peer remembered for Lifecycle KickAnswerer after Accept (peek; cleared on Leave). */
  void ClearPendingAnswererKick();
  bool PeekPendingAnswererKick(const std::string& call_id, std::string* peer_out) const;

  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();
  /** No sweep, no writes (UI reads): expired invites are skipped instead. */
  Roe<std::optional<PendingCallInvite>> PeekTopPendingInvite() const;

  Roe<void> HandleInboundInvite(const std::string& detail_json, const std::string& sender_identity,
                                const ThreadMessage& message, std::optional<int64_t> relay_created_at_ms,
                                std::optional<int64_t> relay_server_time_ms, const std::string& local_identity);
  Roe<void> HandleInboundAccept(const std::string& detail_json, const std::string& sender_identity,
                                const std::string& local_identity);
  /**
   * B30: the answerer's call-media hello reached us before its CallAccept (late relay). For a 1:1
   * call we started whose remote is still invited, treat it as the accept (idempotent with the
   * real one, which may still arrive). No-op otherwise.
   */
  Roe<void> ApplyImplicitAccept(const std::string& call_id, const std::string& identity, const std::string& peer_id,
                                const std::string& local_identity);
  Roe<void> HandleInboundDecline(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundLeave(const std::string& detail_json, const std::string& sender_identity,
                               const std::string& local_identity);
  Roe<void> HandleInboundRoster(const std::string& detail_json);
  Roe<void> HandleInboundSfuAttach(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundSfuAttachFailed(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundHopRefuse(const std::string& detail_json);
  Roe<void> HandleInboundVideoRefresh(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundEnded(const std::string& detail_json, const std::string& local_identity);

private:
  /**
   * `co_invitees`: everyone StartCall is inviting now. Their rows are written only after each
   * invite is on the wire (V045), so the roster snapshot lists them as Invited explicitly — every
   * invitee sees the whole invite list (V050), not the part sent before it.
   */
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity,
                              const std::vector<std::string>& co_invitees);
  /** Remote accepted (CallAccept, or implicitly by its media hello): join, key, media kickoff. */
  Roe<void> ApplyRemoteAccept(const CallAcceptDetail& accept, const std::string& identity,
                              const std::string& local_identity, bool implicit);
  Roe<void> ContinueAcceptAfterPark(const std::string& call_id, InitiationChargeDecision charge_decision,
                                    bool voice_only_accept, const std::string& local_identity);
  // --- StartCall steps ---
  /** Invitees given, no ring pending, a direct / group thread, payable: the origin thread. */
  Roe<Thread> CheckCanStartCall(const std::string& origin_thread_id, const std::vector<std::string>& invitee_identities,
                                const std::string& local_identity);
  /** A new call id + first media key, the session and our Joined row (on disk only). */
  Roe<CallSession> CreatePlacedSession(const Thread& thread, bool video_allowed, const std::string& local_identity);
  /** History, planned hop, invite everyone; then end the current call and admit the new one (Deciding). */
  Roe<void> PlaceCall(CallSession& session, const std::vector<std::string>& invitee_identities,
                      const std::string& local_identity);
  /** "Call started" in the origin thread's history. */
  Roe<void> AppendCallStarted(const CallSession& session);
  /** Invite every invitee (control thread warmed first), then prefetch their reach. */
  Roe<void> InviteAll(const std::string& call_id, const std::vector<std::string>& invitee_identities,
                      const std::string& local_identity);
  // --- InviteParticipant steps ---
  /** Not ended, room to join, and a hop for a third participant (V021). */
  Roe<void> CheckCanInvite(const CallSession& session);
  /** The CallInvite for `invitee_identity`: roster + co-invitees, wrapped media key, listen addrs, caps, offer. */
  Roe<CallInviteDetail> BuildInvite(const CallSession& session, const std::string& local_identity,
                                    const std::string& invitee_identity, const std::vector<std::string>& co_invitees,
                                    int64_t expires_at);
  /** The invite is on the wire: the invitee Ringing, its pending row, the offer booked, the LiveCall's peer. */
  Roe<void> RecordInviteSent(const CallSession& session, const CallInviteDetail& invite);
  // --- HandleInboundInvite steps ---
  /** Already joined / ended here, an offer below our floor (declined), or a stale replay. */
  bool ShouldIgnoreInvite(const CallInviteDetail& invite, const std::string& sender_identity,
                          const std::string& local_identity, std::optional<int64_t> relay_created_at_ms,
                          std::optional<int64_t> relay_server_time_ms);
  /** The pending invite and the Ringing session (re-armed TTL); probes the planned hop. */
  Roe<CallSession> StoreRingingInvite(const CallInviteDetail& invite, const std::string& inviter,
                                      const ThreadMessage& message, const std::string& local_identity);
  /** The invite's roster, the inviter Joined, self Ringing. */
  void SeedInviteRoster(const CallInviteDetail& invite, const std::string& inviter, const CallSession& session,
                        const std::string& local_identity);
  /** The inviter's listen addrs, mesh PeerId, caps; prefetch its reach. */
  void NoteInviterReach(const CallInviteDetail& invite, const std::string& inviter);
  // --- ApplyRemoteAccept steps ---
  /** The accepter's mesh PeerId, and (a real accept) its caps. */
  void NoteAccepterReach(const CallAcceptDetail& accept, const std::string& identity, bool implicit);
  /** The accepter Joined, the session moved on (and narrowed for a voice answer); the stored session. */
  Roe<std::optional<CallSession>> CommitRemoteJoin(const CallAcceptDetail& accept, const std::string& identity,
                                                   bool implicit);
  /** Send the key, note the hop report, pick the path (1:1 schedules the offerer's start); roster after. */
  void StartMediaAfterRemoteAccept(const CallAcceptDetail& accept, const std::string& identity,
                                   const std::string& local_identity, bool implicit);
  // --- ContinueAcceptAfterPark steps ---
  /** The pending invite this device may accept (not expired, not a legacy broadcast row). */
  Roe<PendingCallInvite> LoadAcceptableInvite(const std::string& call_id, const std::string& local_identity);
  /** The stored session, or one built from the invite. */
  CallSession SessionRowForAccept(const PendingCallInvite& pending);
  /** Room to join, and the call not ended meanwhile. */
  Roe<void> CheckAcceptJoinable(const std::string& call_id);
  /** Build and send our CallAccept to `inviter` (listen addrs, caps, hop report, pricing). */
  Roe<CallAcceptDetail> SendCallAccept(const std::string& call_id, const std::string& local_identity,
                                       const std::string& inviter, bool narrow_to_voice, int64_t offer_minor,
                                       InitiationChargeDecision charge_decision);
  /** Joined in the store and on the LiveCall. */
  Roe<void> CommitLocalJoin(CallSession& row, const std::string& local_identity);
  /** Another accept / a leave moved on while our CallAccept was on the wire: leave this one. */
  bool AcceptSuperseded(const std::string& call_id);
  /** The call's media coordinator picks the path; a 1:1 call schedules the answerer's start. */
  void ArmMediaAfterAccept(CallSession& row, const std::string& inviter, const CallAcceptDetail& accept);
  void PostRosterAfterAccept(const std::string& call_id, const std::string& inviter, const std::string& local_identity);
  IThreadStore& store_;
  CallSessionStore& sessions_;
  /** The call's media keys between the peers (owned by CallSessionManager). */
  CallMediaKeyExchange& key_exchange_;
  /** P001 initiation pricing on invite / accept (owned by CallSessionManager). */
  CallInitiationBilling& billing_;
  /** The calls live on this device (owned by CallSessionManager); driven where the store rows change. */
  LiveCalls& live_calls_;
  HostPorts host_;
  DeferredSelf deferred_;
  InitiationChargeDecision pending_accept_charge_ = InitiationChargeDecision::Waive;
  bool pending_accept_charge_set_ = false;
  bool pending_accept_voice_only_ = false;
  std::string pending_answerer_kick_call_id_;
  std::string pending_answerer_kick_peer_;
};

} // namespace pbr
