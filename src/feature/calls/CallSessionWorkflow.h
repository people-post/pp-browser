#pragma once

#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallSummaryLogic.h"
#include "lib/calls/CallTypes.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadRecordTypes.h"
#include "lib/calls/CallSessionStore.h"
#include "feature/calls/CallInitiationBilling.h"
#include "lib/calls/CallMediaKeyExchange.h"
#include "feature/calls/CallSessionEvents.h"
#include "foundation/runtime/OwnerOutbox.h"
#include "foundation/runtime/OwnerSteps.h"
#include "feature/calls/LiveCallMedia.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    std::function<Roe<void>(const std::string& call_id, CallControlType type, const std::string& detail,
                            const std::string& display, const std::string& skip)>
        fan_out_joined;
    std::function<Roe<void>(const std::string& call_id, CallControlType type, const std::string& detail,
                            const std::string& display, const std::string& skip)>
        fan_out_joined_and_ringing;
    /**
     * The call's history line: a summary into its thread (the starter's side writes it); `local_only`
     * keeps it on this device (the app is going down: no sends).
     */
    std::function<Roe<void>(const std::string& thread_id, const CallSummaryDetail& summary, bool local_only,
                            SummaryWrite write)>
        send_summary;
    /**
     * This device's thread for an inbound call: the group's (`group_id`), else the DM with `inviter`.
     * Invites carry no thread id — the caller's is meaningless here.
     */
    std::function<std::optional<std::string>(const std::string& inviter, const std::optional<std::string>& group_id)>
        resolve_call_thread;
    std::function<Roe<CallRosterDetail>(const std::string& call_id)> build_roster_detail;
    /** Who is in a group now; none when unknown. Unset: group membership is not checked. */
    std::function<std::optional<std::set<std::string>>(const std::string& group_id)> group_members;
    std::function<void()> sync_inbox_from_wake;
    std::function<void()> clear_media_activity;
    /** What the call chrome shows the user went wrong (e.g. "That call has ended"). */
    std::function<void(std::string message)> set_last_error;

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
    /** Kick mesh circuit park (composition projects MeshConnectivity rendezvous ReserveOnBootstrapSeeds). */
    std::function<void()> ensure_circuit_ready;
    /**
     * Await circuit-ready (Accept gate): `done(ready)` once parked or at the timeout, from any thread
     * (the workflow reports it as an event). Never blocks the caller.
     */
    std::function<void(int timeout_ms, std::function<void(bool ready)> done)> park_circuit;
    std::function<void(const std::string& relay, const std::string& peer_id)> note_mesh_peer_id_for_relay;
    std::function<std::vector<std::string>()> local_listen_multiaddrs;
    /** projects/privacy T1: our listen addresses go only to peers this allows (unset = all). */
    std::function<bool(const std::string& peer)> may_learn_our_address;
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
                      CallMediaKeyExchange& key_exchange, CallInitiationBilling& billing, LiveCalls& live_calls,
                      LiveCallMedia& call_media);
  ~CallSessionWorkflow() override;

  void SetHostPorts(HostPorts ports);
  /** Where the workflow reports its follow-ups (its parent binds it). */
  void SetOutbox(OwnerOutbox<WorkflowEvent> outbox) { outbox_ = std::move(outbox); }
  /** A follow-up it reported, back from the calls owner's queue. */
  void Handle(WorkflowEvent& event);
  /**
   * Drop the follow-ups still to come (CSM teardown): accepts waiting on their circuit park, and
   * roster fan-outs already reported — they must not run against a store / delivery being torn down.
   */
  void DropFollowUps();

  Roe<CallSession> StartCall(const std::string& origin_thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);
  /**
   * V053: join group call `call_id` this device dropped out of (declined, missed, left): ask everyone
   * it was offered to; one in the call answers with an invite, accepted without ringing. Unanswered
   * within kJoinRequestTimeoutMs: the call is over.
   */
  Roe<CallSession> JoinCall(const std::string& call_id);
  /** V053: the group call this device may join from `thread_id` (a group's thread), if any. */
  std::optional<CallSession> JoinableCallInThread(const std::string& thread_id) const;
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
  /** V053: when this device leaves the group call it waits alone in (0: not waiting). */
  int64_t AloneUntilMs(const std::string& call_id) const;
  /** This device is Joined in the call (not left it). */
  bool StillInCall(const std::string& call_id) const;

  /**
   * A call invite. Returns the call to answer at once: the inviter and this device called each
   * other at the same moment and the inviter's call won (CrossedCall), so ours gave way.
   */
  Roe<std::optional<std::string>> HandleInboundInvite(const std::string& detail_json, const std::string& sender_identity,
                                                      const ThreadMessage& message,
                                                      std::optional<int64_t> relay_created_at_ms,
                                                      std::optional<int64_t> relay_server_time_ms,
                                                      const std::string& local_identity);
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
  /** M021 d5: another of our devices took (or declined) this call — stop it ringing here, no history. */
  Roe<void> StopRingingAnsweredElsewhere(const std::string& call_id, const std::string& local_identity);
  /** V053: someone the call was offered to asks to join — invite them if this device is in it. */
  Roe<void> HandleInboundJoinRequest(const std::string& detail_json, const std::string& sender_identity,
                                     const std::string& local_identity);

private:
  bool IsGroupCall(const CallSession& session) const;
  /** Every participant the call was offered to but this device (any state), one by one. */
  void SendToEveryoneOffered(const std::string& call_id, CallControlType type, const std::string& detail_json,
                             const std::string& display, const std::string& local_identity);
  /** The call is over: those last known in it are marked left (it is no longer joinable). */
  void MarkEveryoneLeft(const std::string& call_id, const std::string& local_identity);
  /** Join requests nobody answered (the call ended); a group call left alone for 5 min. */
  void SweepJoinsAndAloneCalls(const std::string& local_identity, int64_t now);
  /** Whether `identity` is in the group now (true when membership is not checked). */
  bool InGroup(const std::string& group_id, const std::string& identity) const;
  /**
   * V053: who is no longer in the group is no longer in its call — they are taken as gone (the
   * key rotates without them); this device, if it is the one, leaves. Only who this device saw in
   * the group drops: a member it has not heard of yet is not taken for a removed one.
   */
  void DropWhoLeftTheGroup(const std::string& local_identity, int64_t now);
  /** `identity` is no longer in the call (left, or out of its group). */
  Roe<void> ParticipantGone(const std::string& call_id, const std::string& identity,
                            const std::string& local_identity);
  /** V053: a group call's invite that arrived too late to ring: recorded as missed, still joinable. */
  void RecordMissedGroupCall(const CallInviteDetail& invite, const std::string& sender_identity,
                             const std::string& local_identity, int64_t created_at);

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
  /** The starter's summary of a call that just ended (no-op on an invitee's side). */
  void SendSummary(const CallSession& session, LiveCallEndReason reason, SummaryWrite write = SummaryWrite::Final);
  /** Invite every invitee (control thread warmed first), then prefetch their reach. */
  Roe<void> InviteAll(const std::string& call_id, const std::vector<std::string>& invitee_identities,
                      const std::string& local_identity);
  // --- InviteParticipant steps ---
  /** Not ended, room to join, and a hop for a third participant (V021). */
  Roe<void> CheckCanInvite(const CallSession& session);
  /** projects/privacy T1: whether `peer` may get our listen addresses in call signalling. */
  bool MayLearnOurAddress(const std::string& peer) const {
    return !host_.reach.may_learn_our_address || host_.reach.may_learn_our_address(peer);
  }
  /** The CallInvite for `invitee_identity`: roster + co-invitees, wrapped media key, listen addrs, caps, offer. */
  Roe<CallInviteDetail> BuildInvite(const CallSession& session, const std::string& local_identity,
                                    const std::string& invitee_identity, const std::vector<std::string>& co_invitees,
                                    int64_t expires_at);
  /** The invite is on the wire: the invitee Ringing, its pending row, the offer booked, the LiveCall's peer. */
  Roe<void> RecordInviteSent(const CallSession& session, const CallInviteDetail& invite);
  // --- HandleInboundInvite steps ---
  /**
   * This device's own unanswered 1:1 call to `peer`, when `peer` just invited us to another one:
   * both called each other at once. For a group's invite: our call of that group nobody has joined
   * yet (V053: one call per group). Both sides keep the call with the lower id (the same choice on
   * each side, no message needed): the loser's side answers the winner and drops its own.
   */
  std::optional<CallSession> CrossedCall(const CallInviteDetail& invite, const std::string& peer,
                                         const std::string& local_identity) const;
  /** Our call won: their invite never rings, and a redelivery of it is ignored (Ended here). */
  void SwallowCrossedInvite(const CallInviteDetail& invite, const std::string& inviter);
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
  void SendRosterAfterAccept(const workflow_event::RosterAfterAccept& after);
  void SendRosterAfterRemoteAccept(const workflow_event::RosterAfterRemoteAccept& after);
  IThreadStore& store_;
  CallSessionStore& sessions_;
  /** The call's media keys between the peers (owned by CallSessionManager). */
  CallMediaKeyExchange& key_exchange_;
  /** P001 initiation pricing on invite / accept (owned by CallSessionManager). */
  CallInitiationBilling& billing_;
  /** The calls live on this device (owned by CallSessionManager); driven where the store rows change. */
  LiveCalls& live_calls_;
  LiveCallMedia& call_media_;
  HostPorts host_;
  OwnerOutbox<WorkflowEvent> outbox_;
  /** Accepts waiting on their circuit park. */
  OwnerSteps steps_;
  /** Names the follow-ups reported so far; `DropFollowUps` bumps it and the older ones are dropped. */
  uint64_t followup_epoch_ = 0;
  InitiationChargeDecision pending_accept_charge_ = InitiationChargeDecision::Waive;
  bool pending_accept_charge_set_ = false;
  bool pending_accept_voice_only_ = false;
  std::string pending_answerer_kick_call_id_;
  std::string pending_answerer_kick_peer_;
  /** V053: calls this device asked to join → until when it waits for the invite. */
  std::unordered_map<std::string, int64_t> join_requested_until_ms_;
  /** V053: join requests re-sent to anyone in the call (no answer from the earliest joiner). */
  std::unordered_set<std::string> join_any_retry_sent_;
  /** V053: group calls this device is alone in → since when. */
  std::unordered_map<std::string, int64_t> alone_since_ms_;
  /** The last sweep: a long gap means the app slept, and the alone waits start over. */
  int64_t last_alone_sweep_ms_ = 0;
  /** #381: an epoch this device rotated to, resent to the members a few times (no ack on the wire). */
  struct MediaKeyResend {
    uint32_t epoch = 0;
    int64_t rotated_at_ms = 0;
    size_t sent = 0;
  };
  std::unordered_map<std::string, MediaKeyResend> media_key_resends_;
  void ResendRotatedMediaKeys(const std::string& local_identity, int64_t now);
  /** V053: the group's members at the last check, per group call this device is in or rung for. */
  std::unordered_map<std::string, std::set<std::string>> group_members_seen_;
  int64_t group_members_checked_ms_ = 0;
};

} // namespace pbr
