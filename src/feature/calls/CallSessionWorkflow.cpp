#include "feature/calls/CallSessionWorkflow.h"

#include "domain/messaging/CallListenAddrsLogic.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace pbr {

CallSessionWorkflow::CallSessionWorkflow(IThreadStore& store, CallSessionStore& sessions,
                                         CallMediaKeyExchange& key_exchange, CallInitiationBilling& billing,
                                         LiveCalls& live_calls)
    : store_(store), sessions_(sessions), key_exchange_(key_exchange), billing_(billing),
      live_calls_(live_calls) {
  redirectLogger("CallSessionWorkflow");
}

CallSessionWorkflow::~CallSessionWorkflow() = default;

void CallSessionWorkflow::DropWaitingSteps() {
  steps_.DropAll();
}

void CallSessionWorkflow::SetHostPorts(HostPorts ports) {
  host_ = std::move(ports);
}

void CallSessionWorkflow::ClearPendingAnswererKick() {
  pending_answerer_kick_call_id_.clear();
  pending_answerer_kick_peer_.clear();
}

bool CallSessionWorkflow::PeekPendingAnswererKick(const std::string& call_id, std::string* peer_out) const {
  if (pending_answerer_kick_call_id_ != call_id || pending_answerer_kick_peer_.empty()) {
    return false;
  }
  if (peer_out) {
    *peer_out = pending_answerer_kick_peer_;
  }
  return true;
}

Roe<std::optional<CallSession>> CallSessionWorkflow::ActiveLocalCall() const {
  if (!host_.IsBound()) {
    return Error("Call session workflow host ports not bound");
  }
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto active = sessions_.ListActiveSessions();
  if (!active) {
    return active.error();
  }
  for (const CallSession& session : *active) {
    auto participant = sessions_.FindParticipant(session.call_id, *local);
    if (participant && participant->has_value() && (*participant)->state == CallParticipantState::Joined) {
      return std::optional<CallSession>{session};
    }
  }
  return std::optional<CallSession>{};
}

Roe<std::optional<PendingCallInvite>> CallSessionWorkflow::TopPendingInvite() {
  if (!host_.IsBound()) {
    return Error("Call session workflow host ports not bound");
  }
  // Match CSM ListPendingInvites / StartCall gate — expire before reading "top".
  SweepExpiredInvites();
  return PeekTopPendingInvite();
}

Roe<std::optional<PendingCallInvite>> CallSessionWorkflow::PeekTopPendingInvite() const {
  if (!host_.IsBound()) {
    return Error("Call session workflow host ports not bound");
  }
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto pending = sessions_.ListPendingInvitesForInvitee(*local);
  if (!pending) {
    return pending.error();
  }
  // Read-only (callable off the owner): expired rows are skipped here, swept on the owner.
  const int64_t now = util::NowUnixMs();
  std::vector<PendingCallInvite> open;
  for (const PendingCallInvite& invite : *pending) {
    if (invite.status == "pending" && !CallSessionLogic::IsInviteExpired(invite, now)) {
      open.push_back(invite);
    }
  }
  if (open.empty()) {
    return std::optional<PendingCallInvite>{};
  }
  return std::optional<PendingCallInvite>{open.front()};
}

Roe<void> CallSessionWorkflow::LeaveCallIfActiveExcept(const std::string& keep_call_id) {
  // One active call: leave every session this device is joined in but `keep_call_id` (normally at
  // most one) — whichever order the store lists them in.
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto active = sessions_.ListActiveSessions();
  if (!active) {
    return active.error();
  }
  for (const CallSession& session : *active) {
    if (session.call_id == keep_call_id) {
      continue;
    }
    auto self = sessions_.FindParticipant(session.call_id, *local);
    if (!self || !self->has_value() || (*self)->state != CallParticipantState::Joined) {
      continue;
    }
    if (auto left = LeaveCall(session.call_id, LiveCallEndReason::Superseded); !left) {
      log().warning << "LeaveCallIfActiveExcept failed for " << session.call_id << ": " << left.error().message;
      return left.error();
    }
  }
  return {};
}

namespace {

/** Join stamp the roster carries for `identity` (the sender's clock), if any. */
std::optional<int64_t> RosterJoinStamp(const std::vector<CallRosterEntry>& participants, const std::string& identity) {
  for (const CallRosterEntry& entry : participants) {
    if (entry.identity == identity && entry.joined_at) {
      return entry.joined_at;
    }
  }
  return std::nullopt;
}

/** Old peers send no stamps: this device's receipt time (still earlier than later acceptors). */
int64_t LocalJoinStampFallback(const CallSession& session) {
  return session.created_at > 0 ? session.created_at : 1;
}

size_t CountDistinctInvitees(const std::vector<std::string>& invitees, const std::string& local_identity) {
  std::vector<std::string> seen;
  for (const std::string& identity : invitees) {
    if (!identity.empty() && identity != local_identity &&
        std::find(seen.begin(), seen.end(), identity) == seen.end()) {
      seen.push_back(identity);
    }
  }
  return seen.size();
}

/** Roster entries for invitees the store has no row for yet (V050 full invite roster). */
void AppendInvitedCoInvitees(std::vector<CallRosterEntry>& participants, const std::vector<std::string>& co_invitees,
                             const std::string& local_identity) {
  for (const std::string& identity : co_invitees) {
    if (identity.empty() || identity == local_identity) {
      continue;
    }
    const bool listed = std::any_of(participants.begin(), participants.end(),
                                    [&identity](const CallRosterEntry& e) { return e.identity == identity; });
    if (!listed) {
      CallRosterEntry entry;
      entry.identity = identity;
      entry.state = CallParticipantState::Invited;
      participants.push_back(std::move(entry));
    }
  }
}

} // namespace

Roe<CallSession> CallSessionWorkflow::StartCall(const std::string& origin_thread_id, const bool video_allowed,
                                               const std::vector<std::string>& invitee_identities) {
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto thread = CheckCanStartCall(origin_thread_id, invitee_identities, *local);
  if (!thread) {
    return thread.error();
  }
  auto session = CreatePlacedSession(*thread, video_allowed, *local);
  if (!session) {
    return session.error();
  }
  // From here the new call exists on disk: a step that fails ends it, so nothing is left ringing.
  if (auto placed = PlaceCall(*session, invitee_identities, *local); !placed) {
    log().warning << "StartCall failed call_id=" << session->call_id << " err=" << placed.error().message;
    (void)LeaveCall(session->call_id, LiveCallEndReason::StartFailed);
    return placed.error();
  }
  host_.wire.notify_ring_changed();
  return session;
}

Roe<void> CallSessionWorkflow::PlaceCall(CallSession& session, const std::vector<std::string>& invitee_identities,
                                         const std::string& local_identity) {
  if (auto noted = AppendCallStarted(session); !noted) {
    return noted.error();
  }
  // V050: plan the group hop from everyone invited (not attached until the third join).
  if (CountDistinctInvitees(invitee_identities, local_identity) >= 2 && host_.hop.plan_hop_for_invitees) {
    if (auto planned = host_.hop.plan_hop_for_invitees(invitee_identities, local_identity)) {
      session.planned_hop = std::move(planned);
      if (auto saved = sessions_.UpsertSession(session); !saved) {
        return saved.error();
      }
    }
  }
  // Offerer: kick circuit readiness early so StartBridge near-leg is warm by Accept.
  if (host_.reach.ensure_circuit_ready) {
    host_.reach.ensure_circuit_ready();
  }
  // The invites go out first: a start that cannot reach its invitees leaves the current call alone.
  // (An accept is handled on this owner thread, after this returns — never before the admission.)
  if (auto invited = InviteAll(session.call_id, invitee_identities, local_identity); !invited) {
    return invited.error();
  }
  // One active call: placing a call ends the one this device is in (as accepting another does).
  if (auto cleared = LeaveCallIfActiveExcept(session.call_id); !cleared) {
    return cleared.error();
  }
  std::vector<std::string> peers;
  for (const std::string& invitee : invitee_identities) {
    if (invitee != local_identity) {
      peers.push_back(invitee);
    }
  }
  live_calls_.AdmitPlaced(session.call_id, peers);
  live_calls_.NoteOutboundStarted(session.call_id);  // Deciding: no path planner armed yet
  return {};
}

Roe<Thread> CallSessionWorkflow::CheckCanStartCall(const std::string& origin_thread_id,
                                                   const std::vector<std::string>& invitee_identities,
                                                   const std::string& local_identity) {
  if (invitee_identities.empty()) {
    return Error("At least one invitee required");
  }
  auto pending = TopPendingInvite();
  if (!pending) {
    return pending.error();
  }
  if (pending->has_value()) {
    return Error("Decline the incoming call first");
  }
  auto thread = store_.GetThread(origin_thread_id);
  if (!thread || !*thread) {
    return Error("Origin thread not found");
  }
  if ((*thread)->kind != ThreadKind::Direct && (*thread)->kind != ThreadKind::Group) {
    return Error("Calls require a direct or group thread");
  }
  // P001: block outbound dial when initiation offer > 0 and payment rails unavailable.
  if (auto payable = billing_.CheckCanPlace(invitee_identities, local_identity); !payable) {
    return payable.error();
  }
  return **thread;
}

Roe<CallSession> CallSessionWorkflow::CreatePlacedSession(const Thread& thread, const bool video_allowed,
                                                          const std::string& local_identity) {
  const std::string call_id = GenerateCallId();
  auto media_key = key_exchange_.Mint(call_id, 1);
  if (!media_key) {
    return media_key.error();
  }
  const int64_t now = util::NowUnixMs();
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = thread.id;
  if (thread.kind == ThreadKind::Group && thread.group_id) {
    session.origin_group_id = *thread.group_id;
  }
  session.media_mode = video_allowed ? CallMediaMode::Video : CallMediaMode::Voice;
  session.video_allowed = video_allowed;
  session.state = CallSessionState::Ringing;
  session.created_at = now;
  session.media_epoch = media_key->epoch;
  session.media_key_id = media_key->key_id;
  if (auto saved = sessions_.UpsertSession(session); !saved) {
    return saved.error();
  }
  CallParticipant self;
  self.call_id = call_id;
  self.identity = local_identity;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  self.joined_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }
  return session;
}

Roe<void> CallSessionWorkflow::AppendCallStarted(const CallSession& session) {
  CallStartedDetail started;
  started.call_id = session.call_id;
  started.media_mode = session.media_mode;
  started.video_allowed = session.video_allowed;
  auto detail = CallControlCodec::EncodeStarted(started);
  if (!detail) {
    return detail.error();
  }
  const std::string text = session.video_allowed ? "Video call started" : "Voice call started";
  return host_.wire.append_origin_history(session.origin_thread_id.value_or(""), CallControlType::CallStarted,
                                          text, *detail);
}

Roe<void> CallSessionWorkflow::InviteAll(const std::string& call_id, const std::vector<std::string>& invitee_identities,
                                         const std::string& local_identity) {
  for (const std::string& invitee : invitee_identities) {
    if (invitee.empty() || invitee == local_identity) {
      continue;
    }
    // Catalog warm before Invite / SoftMigrate fan-out (thread + directory create off the critical path).
    if (host_.wire.ensure_control_thread) {
      if (auto warmed = host_.wire.ensure_control_thread(invitee); !warmed) {
        log().warning << "Call control thread warm failed peer=" << invitee << " err=" << warmed.error().message;
      }
    }
    if (auto invited = InviteParticipant(call_id, invitee, invitee_identities); !invited) {
      return invited.error();
    }
  }
  if (host_.reach.prefetch_reach) {
    for (const std::string& invitee : invitee_identities) {
      if (!invitee.empty()) {
        host_.reach.prefetch_reach(invitee);
      }
    }
  }
  return {};
}

Roe<void> CallSessionWorkflow::InviteParticipant(const std::string& call_id, const std::string& invitee_identity) {
  return InviteParticipant(call_id, invitee_identity, {});
}


Roe<void> CallSessionWorkflow::InviteParticipant(const std::string& call_id, const std::string& invitee_identity,
                                                 const std::vector<std::string>& co_invitees) {
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  if (auto invitable = CheckCanInvite(**session); !invitable) {
    return invitable.error();
  }
  const int64_t expires_at = util::NowUnixMs() + kDefaultCallInviteTtlMs;
  auto invite = BuildInvite(**session, *local, invitee_identity, co_invitees, expires_at);
  if (!invite) {
    return invite.error();
  }
  auto detail = CallControlCodec::EncodeInvite(*invite);
  if (!detail) {
    return detail.error();
  }
  const std::string display =
      (*session)->media_mode == CallMediaMode::Video ? "Incoming video call" : "Incoming voice call";
  // Wire first — do not leave Ringing/pending debris if send fails.
  if (auto sent = host_.wire.send_direct(invitee_identity, CallControlType::CallInvite, *detail, display); !sent) {
    return sent;
  }
  if (auto recorded = RecordInviteSent(**session, *invite); !recorded) {
    return recorded.error();
  }
  log().info << "CallInvite sent call_id=" << call_id << " peer=" << invitee_identity
             << " media_key_embedded=" << (invite->wrapped_key_b64.empty() ? 0 : 1)
             << " listen_addrs=" << invite->listen_multiaddrs.size();
  return {};
}

Roe<void> CallSessionWorkflow::CheckCanInvite(const CallSession& session) {
  if (session.state == CallSessionState::Ended) {
    return Error("Call has ended");
  }
  auto joined = sessions_.CountJoined(session.call_id);
  if (!joined) {
    return joined.error();
  }
  if (!CallSessionLogic::CanAcceptJoin(*joined)) {
    return Error("Call is full");
  }
  // N≥3 requires media_relay soft-migrate (V021). Refuse mid-call guest invites when no hop
  // exists — otherwise Mac/Linux stay on 1:1 P2P while the invitee hangs on Connecting….
  const bool already_on_sfu = (host_.hop.is_on_sfu_for_call && host_.hop.is_on_sfu_for_call(session.call_id)) ||
                              (session.sfu_hint && !session.sfu_hint->empty());
  if (*joined >= 2 && !already_on_sfu &&
      !(host_.hop.has_media_relay_hop_candidates && host_.hop.has_media_relay_hop_candidates())) {
    return Error("Adding a guest needs call hosting help (enable Help host calls on a computer that's helping the network)");
  }
  return {};
}

Roe<CallInviteDetail> CallSessionWorkflow::BuildInvite(const CallSession& session, const std::string& local_identity,
                                                       const std::string& invitee_identity,
                                                       const std::vector<std::string>& co_invitees,
                                                       const int64_t expires_at) {
  const std::string& call_id = session.call_id;
  CallInviteDetail invite;
  invite.call_id = call_id;
  invite.inviter_identity = local_identity;
  invite.invitee_identity = invitee_identity;
  invite.media_mode = session.media_mode;
  invite.video_allowed = session.video_allowed;
  invite.origin_thread_id = session.origin_thread_id;
  invite.origin_group_id = session.origin_group_id;
  invite.sfu_hint = session.sfu_hint;
  invite.planned_hop = session.planned_hop;
  invite.expires_at = expires_at;
  if (host_.wire.build_roster_detail) {
    if (auto roster = host_.wire.build_roster_detail(call_id); roster) {
      invite.participants = std::move(roster->participants);
    }
  }
  AppendInvitedCoInvitees(invite.participants, co_invitees, local_identity);
  if (host_.reach.prefetch_reach) {
    host_.reach.prefetch_reach(invitee_identity);
  }
  // Catalog + AutoKey before media-key wrap so CallInvite can embed wrapped_key_b64.
  if (host_.wire.ensure_control_thread) {
    if (auto warmed = host_.wire.ensure_control_thread(invitee_identity); !warmed) {
      log().warning << "CallInvite control thread warm failed peer=" << invitee_identity
                    << " err=" << warmed.error().message;
    }
  }
  // Embed epoch key in invite — separate CallMediaKey inbox rows are often ingested without
  // call-control side effects (BenignDuplicate / classifier), so Accept never sees the key.
  invite.media_epoch = session.media_epoch;
  invite.media_key_id = session.media_key_id;
  if (auto key = key_exchange_.Current(call_id); key) {
    invite.wrapped_key_b64 = key_exchange_.WrapForPeer(call_id, *key, invitee_identity);
  }
  if (host_.reach.local_listen_multiaddrs) {
    FillCallListenFields(host_.reach.local_listen_multiaddrs(), invite.libp2p_peer_id, invite.listen_multiaddrs);
  }
  // Explicit mesh PeerId wins over /p2p/ suffix derived from listen MAs.
  if (host_.reach.local_mesh_peer_id) {
    if (const std::string pid = host_.reach.local_mesh_peer_id(); !pid.empty()) {
      invite.libp2p_peer_id = pid;
    }
  }
  if (host_.reach.local_peer_caps) {
    invite.caps = host_.reach.local_peer_caps();
    invite.caps.present = true;
  }
  if (auto offered = billing_.FillInviteOffer(invitee_identity, invite); !offered) {
    return offered.error();
  }
  return invite;
}

Roe<void> CallSessionWorkflow::RecordInviteSent(const CallSession& session, const CallInviteDetail& invite) {
  CallParticipant participant;
  participant.call_id = session.call_id;
  participant.identity = invite.invitee_identity;
  participant.state = CallParticipantState::Ringing;
  if (auto saved = sessions_.UpsertParticipant(participant); !saved) {
    return saved.error();
  }
  PendingCallInvite pending;
  pending.call_id = session.call_id;
  pending.inviter_identity = invite.inviter_identity;
  pending.invitee_identity = invite.invitee_identity;
  pending.media_mode = session.media_mode;
  pending.video_allowed = session.video_allowed;
  pending.origin_thread_id = session.origin_thread_id;
  pending.origin_group_id = session.origin_group_id;
  pending.sfu_hint = session.sfu_hint;
  pending.expires_at = invite.expires_at;
  pending.created_at = util::NowUnixMs();
  pending.status = "pending";
  if (auto saved = sessions_.UpsertPendingInvite(pending); !saved) {
    return saved.error();
  }
  billing_.NoteInviteSent(invite.invitee_identity, invite);
  live_calls_.AddPeer(session.call_id, invite.invitee_identity);
  return {};
}

void CallSessionWorkflow::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  pending_accept_charge_ = decision;
  pending_accept_charge_set_ = true;
}

void CallSessionWorkflow::SetPendingAcceptVoiceOnly(const bool voice_only) {
  pending_accept_voice_only_ = voice_only;
}

void CallSessionWorkflow::AcceptInviteAsync(const std::string& call_id, InitiationChargeDecision charge_decision,
                                            std::function<void(Roe<void>)> on_done) {
  if (pending_accept_charge_set_) {
    charge_decision = pending_accept_charge_;
    pending_accept_charge_set_ = false;
    pending_accept_charge_ = InitiationChargeDecision::Waive;
  }
  const bool voice_only_accept = pending_accept_voice_only_;
  pending_accept_voice_only_ = false;
  log().info << "AcceptInvite start call_id=" << call_id
             << " charge=" << InitiationChargeDecisionToWire(charge_decision);
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << local.error().message;
    on_done(local.error());
    return;
  }
  SweepExpiredInvites();
  if (auto cleared = LeaveCallIfActiveExcept(call_id); !cleared) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << cleared.error().message;
    on_done(cleared.error());
    return;
  }
  live_calls_.MarkAccepting(call_id);
  // An accept that does not go through leaves the call ringing (lifecycle AcceptFailed → Ringing).
  on_done = [this, call_id, on_done = std::move(on_done)](Roe<void> accepted) {
    if (!accepted) {
      live_calls_.MarkAcceptFailed(call_id);
    }
    on_done(std::move(accepted));
  };
  // Thin product gate: connectivity owns the park; the session waits for it before CallAccept —
  // asynchronously, so the calls owner keeps serving inbound control / Leave meanwhile.
  if (!host_.reach.park_circuit) {
    on_done(ContinueAcceptAfterPark(call_id, charge_decision, voice_only_accept, *local));
    return;
  }
  const uint64_t step = steps_.StoreFor<bool>(
      [this, call_id, charge_decision, voice_only_accept, local = *local, on_done](bool ready) {
        log().info << "AcceptInvite circuit park call_id=" << call_id << " ready=" << (ready ? 1 : 0);
        on_done(ContinueAcceptAfterPark(call_id, charge_decision, voice_only_accept, local));
      });
  host_.reach.park_circuit(12000, [outbox = outbox_, step](bool ready) {
    outbox.Emit(workflow_event::Continue{OwnerStepReady{step, std::make_shared<std::any>(ready)}});
  });
}

Roe<void> CallSessionWorkflow::ContinueAcceptAfterPark(const std::string& call_id,
                                                      InitiationChargeDecision charge_decision,
                                                      bool voice_only_accept,
                                                      const std::string& local_identity) {
  // LeaveCallIfActiveExcept only sees Joined sessions; an Ended prior call can leave the engine
  // running. Stop it before this call's media — never the call we are accepting.
  live_calls_.StopMediaExcept(call_id);
  auto pending = LoadAcceptableInvite(call_id, local_identity);
  if (!pending) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << pending.error().message;
    return pending.error();
  }
  const std::string inviter = pending->inviter_identity;
  const int64_t offer_minor = billing_.OfferFrom(inviter);
  if (auto payable = CallInitiationBilling::CheckCanAccept(charge_decision, offer_minor); !payable) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=payment_unavailable take_all";
    return payable.error();
  }
  CallSession row = SessionRowForAccept(*pending);
  // video-voice-choice: callee answered a video invite as voice-only — narrow this session and
  // echo it on the wire so the caller (1:1) narrows too. Never widens (video_allowed already
  // false is a no-op).
  const bool narrow_to_voice = voice_only_accept && row.video_allowed;
  if (narrow_to_voice) {
    row.video_allowed = false;
  }
  if (auto joinable = CheckAcceptJoinable(call_id); !joinable) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << joinable.error().message;
    return joinable.error();
  }
  // CallAccept on the wire before Joined / chrome / planner arm — a failed send must not leave
  // local Joined or SoftMigrate half-started.
  auto accept = SendCallAccept(call_id, local_identity, inviter, narrow_to_voice, offer_minor, charge_decision);
  if (!accept) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << accept.error().message;
    return accept.error();
  }
  if (auto joined = CommitLocalJoin(row, local_identity); !joined) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << joined.error().message;
    return joined.error();
  }
  if (AcceptSuperseded(call_id)) {
    return Error("Accept superseded");
  }
  ArmMediaAfterAccept(row, inviter, *accept);
  PostRosterAfterAccept(call_id, inviter, local_identity);
  log().info << "AcceptInvite end call_id=" << call_id << " ok";
  return {};
}

Roe<PendingCallInvite> CallSessionWorkflow::LoadAcceptableInvite(const std::string& call_id,
                                                                const std::string& local_identity) {
  auto pending = sessions_.LoadPendingInvite(call_id, local_identity);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    return Error("Pending call invite not found");
  }
  if (CallSessionLogic::IsInviteExpired(**pending, util::NowUnixMs())) {
    (void)sessions_.UpdateInviteStatus(call_id, local_identity, "expired");
    host_.wire.notify_ring_changed();
    return Error("Call invite expired");
  }
  if (IsBroadcastSession((*pending)->session_kind)) {
    // Legacy rows only (pre-feature/broadcast builds): a live broadcast is watched from its
    // announce through BroadcastHub, never accepted as a call.
    return Error("Live broadcasts are watched from the announce, not accepted as calls");
  }
  return **pending;
}

CallSession CallSessionWorkflow::SessionRowForAccept(const PendingCallInvite& pending) {
  if (auto session = sessions_.LoadSession(pending.call_id); session && session->has_value()) {
    return **session;
  }
  CallSession row;
  row.call_id = pending.call_id;
  row.origin_thread_id = pending.origin_thread_id;
  row.origin_group_id = pending.origin_group_id;
  row.media_mode = pending.media_mode;
  row.video_allowed = pending.video_allowed;
  row.state = CallSessionState::Ringing;
  row.created_at = pending.created_at;
  row.media_epoch = 1;
  row.sfu_hint = pending.sfu_hint;
  return row;
}

Roe<void> CallSessionWorkflow::CheckAcceptJoinable(const std::string& call_id) {
  auto joined = sessions_.CountJoined(call_id);
  if (!CallSessionLogic::CanAcceptJoin(joined ? *joined : 0)) {
    return Error("Call is full");
  }
  // Concurrent Accept B → LeaveCallIfActiveExcept may End this call while we are still on the
  // AcceptInvite(A) worker. Do not resurrect an Ended row as Joined.
  if (auto latest = sessions_.LoadSession(call_id); latest && latest->has_value() &&
      (*latest)->state == CallSessionState::Ended) {
    return Error("Call already ended");
  }
  return {};
}

Roe<CallAcceptDetail> CallSessionWorkflow::SendCallAccept(const std::string& call_id,
                                                          const std::string& local_identity,
                                                          const std::string& inviter, const bool narrow_to_voice,
                                                          const int64_t offer_minor,
                                                          const InitiationChargeDecision charge_decision) {
  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = local_identity;
  accept.video_enabled = false;
  if (narrow_to_voice) {
    accept.video_allowed = false;
  }
  billing_.FillAccept(offer_minor, charge_decision, accept);
  if (host_.reach.local_listen_multiaddrs) {
    FillCallListenFields(host_.reach.local_listen_multiaddrs(), accept.libp2p_peer_id, accept.listen_multiaddrs);
  }
  if (host_.reach.local_mesh_peer_id) {
    if (const std::string pid = host_.reach.local_mesh_peer_id(); !pid.empty()) {
      accept.libp2p_peer_id = pid;
    }
  }
  if (host_.reach.local_peer_caps) {
    accept.caps = host_.reach.local_peer_caps();
    accept.caps.present = true;
  }
  if (host_.hop.hop_report_for_accept) {
    accept.hop_report = host_.hop.hop_report_for_accept(call_id);
  }
  auto detail = CallControlCodec::EncodeAccept(accept);
  if (!detail) {
    return detail.error();
  }
  if (auto sent = host_.wire.send_direct(inviter, CallControlType::CallAccept, *detail, "Call accepted"); !sent) {
    log().warning << "CallAccept send failed call_id=" << call_id << " err=" << sent.error().message;
    return sent.error();
  }
  billing_.NoteAccepted(inviter);
  return accept;
}

Roe<void> CallSessionWorkflow::CommitLocalJoin(CallSession& row, const std::string& local_identity) {
  row.state = CallSessionLogic::TransitionOnRemoteJoined(row.state);
  if (auto saved = sessions_.UpsertSession(row); !saved) {
    return saved.error();
  }
  CallParticipant self;
  self.call_id = row.call_id;
  self.identity = local_identity;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  // V050 gt2b: no stamp from this device's clock — the inviter stamps our join when it processes the
  // CallAccept and its CallRoster brings the stamp back (the store keeps the earliest).
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }
  (void)sessions_.UpdateInviteStatus(row.call_id, local_identity, "accepted");
  live_calls_.MarkJoined(row.call_id);
  return {};
}

bool CallSessionWorkflow::AcceptSuperseded(const std::string& call_id) {
  // B-CONFLICT: Accept B may have moved on / LeaveCall'd us while CallAccept was on the wire.
  // Do not ScheduleStart or report success for a superseded accept.
  const LiveCall* ours = live_calls_.Find(call_id);
  if (ours && ours->IsOpen() && !live_calls_.HasOtherActive(call_id)) {
    return false;
  }
  log().info << "AcceptInvite superseded after CallAccept call_id=" << call_id
             << " accepting=" << live_calls_.AcceptingCallId();
  if (auto latest = sessions_.LoadSession(call_id);
      latest && latest->has_value() && (*latest)->state != CallSessionState::Ended) {
    (void)LeaveCall(call_id, LiveCallEndReason::Superseded);
  }
  return true;
}

void CallSessionWorkflow::ArmMediaAfterAccept(CallSession& row, const std::string& inviter,
                                              const CallAcceptDetail& accept) {
  // Runs on the calls owner (thread-ownership t2a). Do not wait on ListenOn / PollInbox here.
  // N→planner: Topology for N≥3 / hint; else Bridge Direct (CallMediaPlannerSelectLogic / V038).
  const std::string& call_id = row.call_id;
  auto joined_after = sessions_.CountJoined(call_id);
  const size_t planner_n = joined_after ? *joined_after : 0;  // V050: joined only — ringing invitees never arm the hop
  // The call's media coordinator decides the path (the hop takes a group call; else 1:1 Direct).
  CallMediaCoordinator* call_media = live_calls_.Media(call_id);
  const bool topology_took_media =
      call_media && call_media->DecideOnLocalAccept(planner_n, row.sfu_hint) == CallMediaPath::Hop;
  if (topology_took_media) {
    pending_answerer_kick_call_id_.clear();
    pending_answerer_kick_peer_.clear();
    log().info << "AcceptInvite topology owns media (no ScheduleStart) call_id=" << call_id
               << " planner_n=" << planner_n
               << " sfu_hint=" << (row.sfu_hint && !row.sfu_hint->empty() ? 1 : 0);
    if (host_.wire.notify_ring_changed) {
      host_.wire.notify_ring_changed();
    }
  } else {
    if (row.sfu_hint && !row.sfu_hint->empty()) {
      row.sfu_hint.reset();
      (void)sessions_.UpsertSession(row);
    }
    live_calls_.SetMediaStatus(call_id, CallMediaStatus::DirectConnecting, "ScheduleStartDirect");
    // Drop stale SoftMigrate chrome ("Connecting group media…") from a prior hop attempt.
    if (host_.wire.clear_media_activity) {
      host_.wire.clear_media_activity();
    }
    // Remember peer for the answerer kick — PeerIdentityForCall can lag roster.
    pending_answerer_kick_call_id_ = call_id;
    pending_answerer_kick_peer_ = inviter;
    if (host_.wire.notify_ring_changed) {
      host_.wire.notify_ring_changed();
    }
    // ScheduleStart after CallAccept is on the wire so offerer can arm inbound while we dial.
    log().info << "AcceptInvite → ScheduleStartDirectMedia (answerer) call_id=" << call_id
               << " inviter=" << inviter << " planner_n=" << planner_n
               << " listen_mas=" << accept.listen_multiaddrs.size()
               << " peer_id=" << (accept.libp2p_peer_id.empty() ? 0 : 1);
    if (host_.duplex.schedule_start_direct) {
      host_.duplex.schedule_start_direct(call_id, inviter, false);
    }
  }
  // Pull CallMediaKey ASAP — do not wait for the next UI-tick poll.
  if (host_.wire.sync_inbox_from_wake) {
    host_.wire.sync_inbox_from_wake();
  }
}

void CallSessionWorkflow::PostRosterAfterAccept(const std::string& call_id, const std::string& inviter,
                                                const std::string& local_identity) {
  // Roster / prefetch as the owner's next event, after Accept has reported (no Accept hang UX).
  outbox_.Emit(workflow_event::RosterAfterAccept{call_id, inviter, local_identity});
}

void CallSessionWorkflow::SendRosterAfterAccept(const workflow_event::RosterAfterAccept& after) {
  // Sends only prepare + enqueue (Amp on Mesh I/O, relay fallback on a worker).
  if (!host_.IsBound()) {
    return;
  }
  if (host_.wire.build_roster_detail) {
    if (auto roster = host_.wire.build_roster_detail(after.call_id); roster) {
      if (auto roster_json = CallControlCodec::EncodeRoster(*roster); roster_json) {
        if (host_.wire.send_direct) {
          (void)host_.wire.send_direct(after.inviter, CallControlType::CallRoster, *roster_json, "Call roster");
        }
        if (host_.wire.fan_out_joined_and_ringing) {
          (void)host_.wire.fan_out_joined_and_ringing(after.call_id, CallControlType::CallRoster, *roster_json,
                                                      "Call roster", after.local_identity);
        }
      }
    }
  }
  if (host_.reach.prefetch_reach) {
    host_.reach.prefetch_reach(after.inviter);
  }
}

void CallSessionWorkflow::SendRosterAfterRemoteAccept(const workflow_event::RosterAfterRemoteAccept& after) {
  if (!host_.IsBound()) {
    return;
  }
  if (host_.reach.prefetch_reach) {
    host_.reach.prefetch_reach(after.peer);
  }
  if (auto roster = host_.wire.build_roster_detail(after.call_id); roster) {
    if (auto roster_json = CallControlCodec::EncodeRoster(*roster); roster_json) {
      (void)host_.wire.fan_out_joined_and_ringing(after.call_id, CallControlType::CallRoster, *roster_json,
                                                  "Call roster", after.local_identity);
    }
  }
}

void CallSessionWorkflow::Handle(WorkflowEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, workflow_event::RosterAfterAccept>) {
          SendRosterAfterAccept(e);
        } else if constexpr (std::is_same_v<E, workflow_event::RosterAfterRemoteAccept>) {
          SendRosterAfterRemoteAccept(e);
        } else if constexpr (std::is_same_v<E, workflow_event::Continue>) {
          steps_.Run(e.step.id, e.step.value.get());
        } else {
          static_assert(!sizeof(E), "handle every WorkflowEvent");
        }
      },
      event);
}

Roe<void> CallSessionWorkflow::DeclineInvite(const std::string& call_id) {
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto pending = sessions_.LoadPendingInvite(call_id, *local);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    return Error("Pending call invite not found");
  }

  CallDeclineDetail decline;
  decline.call_id = call_id;
  decline.identity = *local;
  auto detail = CallControlCodec::EncodeDecline(decline);
  if (!detail) {
    return detail.error();
  }
  // Send before clearing pending so UI DrainUntil(pending empty) cannot race mid-send.
  if (!host_.wire.send_direct) {
    return Error("Call session workflow host ports not bound");
  }
  if (auto sent = host_.wire.send_direct((*pending)->inviter_identity, CallControlType::CallDecline, *detail,
                                    "Call declined");
      !sent) {
    return sent.error();
  }

  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = *local;
  participant.state = CallParticipantState::Declined;
  (void)sessions_.UpsertParticipant(participant);
  (void)sessions_.UpdateInviteStatus(call_id, *local, "declined");

  // Drop sticky Accept charge/voice-only if Decline wins the race with a pre-set decision.
  pending_accept_charge_set_ = false;
  pending_accept_charge_ = InitiationChargeDecision::Waive;
  pending_accept_voice_only_ = false;
  if (pending_answerer_kick_call_id_ == call_id) {
    ClearPendingAnswererKick();
  }

  // CALLS: expire/Decline → Idle chrome + durable Ended (same as Sweep expire path).
  auto session = sessions_.LoadSession(call_id);
  if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
    (void)EndCallLocal(**session, std::nullopt, LiveCallEndReason::Declined);
  } else {
    live_calls_.Close(call_id, LiveCallEndReason::Declined);
    if (host_.wire.notify_ring_changed) {
      host_.wire.notify_ring_changed();
    }
  }
  return {};
}

Roe<void> CallSessionWorkflow::MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity) {
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  std::vector<std::string> remaining;
  for (const CallParticipant& row : *participants) {
    if (row.state == CallParticipantState::Joined && row.identity != leaver_identity) {
      remaining.push_back(row.identity);
    }
  }
  if (remaining.empty()) {
    return {};
  }
  auto coordinator = CallSessionLogic::SelectEpochCoordinator(remaining);
  if (!coordinator || *coordinator != *local) {
    return {};
  }

  auto key = key_exchange_.Rotate(call_id);
  if (!key) {
    return key.error();
  }

  auto roster = host_.wire.build_roster_detail(call_id);
  if (!roster) {
    return roster.error();
  }
  roster->media_epoch = key->epoch;
  auto roster_json = CallControlCodec::EncodeRoster(*roster);
  if (!roster_json) {
    return roster_json.error();
  }
  for (const std::string& peer : remaining) {
    if (peer == *local) {
      continue;
    }
    (void)key_exchange_.Send(call_id, peer, *key);
    (void)host_.wire.send_direct(peer, CallControlType::CallRoster, *roster_json, "Call roster");
  }
  return {};
}

Roe<void> CallSessionWorkflow::EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms,
                                          const LiveCallEndReason reason) {
  live_calls_.Close(session.call_id, reason);
  live_calls_.StopMedia(session.call_id);
  session.state = CallSessionState::Ended;
  session.ended_at = util::NowUnixMs();
  if (auto saved = sessions_.UpsertSession(session); !saved) {
    return saved.error();
  }
  if (session.origin_thread_id && host_.wire.append_origin_history) {
    CallEndedDetail ended;
    ended.call_id = session.call_id;
    ended.duration_ms = duration_ms;
    auto detail = CallControlCodec::EncodeEnded(ended);
    if (detail) {
      (void)host_.wire.append_origin_history(*session.origin_thread_id, CallControlType::CallEnded, "Call ended", *detail);
    }
  }
  if (host_.wire.notify_ring_changed) {
    host_.wire.notify_ring_changed();
  }
  return {};
}

Roe<void> CallSessionWorkflow::LeaveCall(const std::string& call_id, const LiveCallEndReason reason) {
  // This device leaves the call whether or not it ends for the others (group).
  live_calls_.Close(call_id, reason);
  if (pending_answerer_kick_call_id_ == call_id) {
    ClearPendingAnswererKick();
  }
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    // Still detach leftover SFU if the disk row is gone but capture is live.
    live_calls_.StopMedia(call_id);
    return Error("Call session not found");
  }
  if ((*session)->state == CallSessionState::Ended) {
    // Session already Ended (remote CallEnded / prior EndCallLocal) must still tear down
    // media_relay — otherwise the next Accept inherits zombie RX and red "reconnecting".
    live_calls_.StopMedia(call_id);
    return {};
  }
  live_calls_.StopMedia(call_id);

  const int64_t now = util::NowUnixMs();
  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Left;
  self.left_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = *local;
  auto detail = CallControlCodec::EncodeLeave(leave);
  if (!detail) {
    return detail.error();
  }
  if (host_.wire.fan_out_joined) {
    (void)host_.wire.fan_out_joined(call_id, CallControlType::CallLeave, *detail, "Left the call", *local);
  }

  auto joined = sessions_.CountJoined(call_id);
  const size_t remaining = joined ? *joined : 0;
  const CallSessionState next = CallSessionLogic::TransitionOnLeave((*session)->state, remaining);
  if (next == CallSessionState::Ended) {
    std::optional<int64_t> duration;
    if ((*session)->created_at > 0) {
      duration = now - (*session)->created_at;
    }
    CallEndedDetail ended;
    ended.call_id = call_id;
    ended.duration_ms = duration;
    auto ended_json = CallControlCodec::EncodeEnded(ended);
    if (ended_json && host_.wire.fan_out_joined_and_ringing) {
      // Notify Ringing/Invited invitees as well so offline inbox delivery can clear stale rings.
      (void)host_.wire.fan_out_joined_and_ringing(call_id, CallControlType::CallEnded, *ended_json, "Call ended",
                                             *local);
    }
    return EndCallLocal(**session, duration, reason);
  }

  (*session)->state = next;
  if (auto saved = sessions_.UpsertSession(**session); !saved) {
    return saved.error();
  }
  (void)MaybeRotateMediaKey(call_id, *local);
  if (host_.wire.notify_ring_changed) {
    host_.wire.notify_ring_changed();
  }
  return {};
}

void CallSessionWorkflow::SweepExpiredInvites() {
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return;
  }
  const int64_t now = util::NowUnixMs();
  bool changed = false;
  if (auto pending = sessions_.ListPendingInvitesForInvitee(*local); pending) {
    for (PendingCallInvite& invite : *pending) {
      if (invite.status != "pending") {
        continue;
      }
      if (!CallSessionLogic::IsInviteExpired(invite, now)) {
        continue;
      }
      (void)sessions_.UpdateInviteStatus(invite.call_id, invite.invitee_identity, "expired");
      CallParticipant participant;
      participant.call_id = invite.call_id;
      participant.identity = invite.invitee_identity;
      participant.state = CallParticipantState::Missed;
      (void)sessions_.UpsertParticipant(participant);
      // CALLS: expire → Idle (same chrome clear as Decline). EndCallLocal → RemoteEnded when
      // lifecycle is still bound to this ringing invite.
      auto session = sessions_.LoadSession(invite.call_id);
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        (void)EndCallLocal(**session, std::nullopt, LiveCallEndReason::Expired);
      } else {
        live_calls_.Close(invite.call_id, LiveCallEndReason::Expired);
      }
      changed = true;
    }
  }

  // CALLS outbound unanswered: clear sticky Calling bar without waiting on GUI LeaveClicked.
  const LiveCall* calling = live_calls_.Active();
  if (calling && calling->State() == LiveCallState::Calling && !live_calls_.MediaRunning()) {
    auto active = ActiveLocalCall();
    if (active && active->has_value() &&
        CallSessionLogic::ShouldAutoLeaveOutboundUnanswered(true, false, (*active)->created_at, now)) {
      const std::string call_id = (*active)->call_id;
      if (calling->Id() == call_id) {
        log().warning << "outbound unanswered timeout call_id=" << call_id;
        if (auto left = LeaveCall(call_id, LiveCallEndReason::Unanswered); !left) {
          log().warning << "outbound unanswered LeaveCall failed call_id=" << call_id
                        << " err=" << left.error().message;
          auto session = sessions_.LoadSession(call_id);
          if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
            (void)EndCallLocal(**session, std::nullopt, LiveCallEndReason::Unanswered);
          }
        }
        return;
      }
    }
  }

  if (changed) {
    host_.wire.notify_ring_changed();
  }
}

void CallSessionWorkflow::AbandonOrphanedCallsAfterRestart() {
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return;
  }

  // Copy ids first — LeaveCall / EndCallLocal mutate the store.
  std::vector<std::string> joined_calls;
  std::vector<CallSession> other_live;
  if (auto active = sessions_.ListActiveSessions(); active) {
    for (const CallSession& session : *active) {
      auto participant = sessions_.FindParticipant(session.call_id, *local);
      if (participant && participant->has_value() &&
          (*participant)->state == CallParticipantState::Joined) {
        joined_calls.push_back(session.call_id);
      } else {
        other_live.push_back(session);
      }
    }
  }

  for (const std::string& call_id : joined_calls) {
    if (auto left = LeaveCall(call_id, LiveCallEndReason::Orphaned); !left) {
      auto session = sessions_.LoadSession(call_id);
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        (void)EndCallLocal(**session, std::nullopt, LiveCallEndReason::Orphaned);
      }
    }
  }
  for (CallSession& session : other_live) {
    if (session.state == CallSessionState::Ended) {
      continue;
    }
    (void)EndCallLocal(session, std::nullopt, LiveCallEndReason::Orphaned);
  }

  if (auto pending = sessions_.ListPendingInvitesForInvitee(*local); pending) {
    for (const PendingCallInvite& invite : *pending) {
      if (invite.status != "pending") {
        continue;
      }
      (void)sessions_.UpdateInviteStatus(invite.call_id, invite.invitee_identity, "expired");
      CallParticipant participant;
      participant.call_id = invite.call_id;
      participant.identity = invite.invitee_identity;
      participant.state = CallParticipantState::Missed;
      (void)sessions_.UpsertParticipant(participant);
      auto session = sessions_.LoadSession(invite.call_id);
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        (void)EndCallLocal(**session, std::nullopt, LiveCallEndReason::Orphaned);
      }
    }
  }

  host_.wire.notify_ring_changed();
}

Roe<void> CallSessionWorkflow::HandleInboundInvite(const std::string& detail_json,
                                                  const std::string& sender_identity,
                                                  const ThreadMessage& message,
                                                  const std::optional<int64_t> relay_created_at_ms,
                                                  const std::optional<int64_t> relay_server_time_ms,
                                                  const std::string& local_identity) {
  auto invite = CallControlCodec::DecodeInvite(detail_json);
  if (!invite) {
    return invite.error();
  }
  if (ShouldIgnoreInvite(*invite, sender_identity, local_identity, relay_created_at_ms, relay_server_time_ms)) {
    return {};
  }
  const std::string inviter = invite->inviter_identity.empty() ? sender_identity : invite->inviter_identity;
  auto session = StoreRingingInvite(*invite, inviter, message, local_identity);
  if (!session) {
    return session.error();
  }
  // Media key embedded in invite (preferred); CallMediaKey message remains a backup.
  (void)key_exchange_.TakeWrapped(invite->call_id, session->media_epoch, invite->media_key_id, invite->wrapped_key_b64,
                                  sender_identity, "CallInvite");
  SeedInviteRoster(*invite, inviter, *session, local_identity);
  NoteInviterReach(*invite, inviter);
  std::vector<std::string> peers{inviter};
  for (const CallRosterEntry& entry : invite->participants) {
    if (entry.identity != local_identity) {
      peers.push_back(entry.identity);
    }
  }
  live_calls_.AdmitInvited(invite->call_id, peers);
  // Answerer: kick circuit readiness on ring (park owned by the shared MeshMediaPlane).
  if (host_.reach.ensure_circuit_ready) {
    host_.reach.ensure_circuit_ready();
  }
  host_.wire.notify_ring_changed();
  return {};
}

bool CallSessionWorkflow::ShouldIgnoreInvite(const CallInviteDetail& invite, const std::string& sender_identity,
                                             const std::string& local_identity,
                                             const std::optional<int64_t> relay_created_at_ms,
                                             const std::optional<int64_t> relay_server_time_ms) {
  // Already joined this call — ignore redelivered / duplicate invites (would demote to Ringing
  // and flicker ring chrome over the in-call banner).
  if (auto existing = sessions_.FindParticipant(invite.call_id, local_identity);
      existing && existing->has_value() && (*existing)->state == CallParticipantState::Joined) {
    log().info << "CallInvite ignored; already joined call_id=" << invite.call_id;
    return true;
  }
  // B24: a replayed invite for a call this device already ended must not re-arm the ring
  // (it hijacked the accept button mid-call and joined a dead call id).
  if (auto ended = sessions_.LoadSession(invite.call_id);
      ended && ended->has_value() && (*ended)->state == CallSessionState::Ended) {
    log().info << "CallInvite ignored (ended session) call_id=" << invite.call_id;
    return true;
  }
  // P001: when we charge (local floor > 0), auto-reject offers below floor.
  if (!billing_.TakeInboundOffer(sender_identity, invite)) {
    CallDeclineDetail decline;
    decline.call_id = invite.call_id;
    decline.identity = local_identity;
    if (auto encoded = CallControlCodec::EncodeDecline(decline)) {
      (void)host_.wire.send_direct(sender_identity, CallControlType::CallDecline, *encoded,
                                   "Call declined (offer too low)");
    }
    return true;
  }
  const int64_t now = util::NowUnixMs();
  if (CallSessionLogic::ShouldDropStaleInvite(invite, now, relay_created_at_ms, relay_server_time_ms)) {
    log().warning << "CallInvite dropped as stale call_id=" << invite.call_id
                  << " expires_at=" << (invite.expires_at ? std::to_string(*invite.expires_at) : "none")
                  << " now=" << now
                  << " relay_created=" << (relay_created_at_ms ? std::to_string(*relay_created_at_ms) : "none")
                  << " relay_now=" << (relay_server_time_ms ? std::to_string(*relay_server_time_ms) : "none");
    return true;
  }
  // Near-live invite: re-arm ring TTL from local receipt (skew-safe). Relay-age gate already
  // dropped long-backlogged inbox rows when create/now samples were present.
  if (CallSessionLogic::IsInviteExpired(invite, now)) {
    log().info << "CallInvite past wire expires_at; re-arming locally call_id=" << invite.call_id
               << " expires_at=" << (invite.expires_at ? std::to_string(*invite.expires_at) : "none")
               << " now=" << now;
  }
  return false;
}

Roe<CallSession> CallSessionWorkflow::StoreRingingInvite(const CallInviteDetail& invite, const std::string& inviter,
                                                         const ThreadMessage& message,
                                                         const std::string& local_identity) {
  const int64_t now = util::NowUnixMs();
  PendingCallInvite pending;
  pending.call_id = invite.call_id;
  pending.inviter_identity = inviter;
  // Always key pending rows to local identity so ListPendingInvitesForInvitee matches.
  pending.invitee_identity = local_identity;
  pending.media_mode = invite.media_mode;
  pending.video_allowed = CallSessionLogic::VideoAllowedFromInvite(invite);
  pending.origin_thread_id = invite.origin_thread_id;
  pending.origin_group_id = invite.origin_group_id;
  pending.sfu_hint = invite.sfu_hint;
  pending.expires_at = now + kDefaultCallInviteTtlMs;
  pending.created_at = message.timestamp > 0 ? message.timestamp : now;
  pending.status = "pending";
  if (auto saved = sessions_.UpsertPendingInvite(pending); !saved) {
    return saved.error();
  }
  CallSession session;
  session.call_id = invite.call_id;
  session.origin_thread_id = invite.origin_thread_id;
  session.origin_group_id = invite.origin_group_id;
  session.media_mode = invite.media_mode;
  session.video_allowed = CallSessionLogic::VideoAllowedFromInvite(invite);
  session.state = CallSessionState::Ringing;
  session.created_at = pending.created_at;
  session.media_epoch = invite.media_epoch > 0 ? invite.media_epoch : 1;
  session.media_key_id = invite.media_key_id;
  session.sfu_hint = invite.sfu_hint;
  session.planned_hop = invite.planned_hop;
  (void)sessions_.UpsertSession(session);
  if (session.planned_hop && host_.hop.probe_invite_hops) {
    host_.hop.probe_invite_hops(session.call_id);  // V050 gt4: check the planned hop while ringing
  }
  return session;
}

void CallSessionWorkflow::SeedInviteRoster(const CallInviteDetail& invite, const std::string& inviter,
                                           const CallSession& session, const std::string& local_identity) {
  // Seed the full roster from the invite when present; the inviter and self in any case.
  for (const CallRosterEntry& entry : invite.participants) {
    if (entry.identity.empty()) {
      continue;
    }
    CallParticipant row;
    row.call_id = invite.call_id;
    row.identity = entry.identity;
    row.state = entry.state;
    row.media.audio_muted = entry.audio_muted;
    row.media.video_enabled = entry.video_enabled;
    if (entry.identity == local_identity) {
      row.state = CallParticipantState::Ringing;
    } else if (entry.identity == inviter && row.state != CallParticipantState::Joined) {
      row.state = CallParticipantState::Joined;
    }
    if (row.state == CallParticipantState::Joined) {
      // V050 gt2b: the inviter's stamp (one clock for everyone) — this device's receipt time only
      // for peers that send none; it still precedes later acceptors (initiator detection).
      row.joined_at = entry.joined_at ? entry.joined_at : std::optional<int64_t>(LocalJoinStampFallback(session));
    }
    (void)sessions_.UpsertParticipant(row);
  }
  CallParticipant inviter_row;
  inviter_row.call_id = invite.call_id;
  inviter_row.identity = inviter;
  inviter_row.state = CallParticipantState::Joined;
  inviter_row.joined_at = RosterJoinStamp(invite.participants, inviter).value_or(LocalJoinStampFallback(session));
  (void)sessions_.UpsertParticipant(inviter_row);
  CallParticipant self;
  self.call_id = invite.call_id;
  self.identity = local_identity;
  self.state = CallParticipantState::Ringing;
  (void)sessions_.UpsertParticipant(self);
}

void CallSessionWorkflow::NoteInviterReach(const CallInviteDetail& invite, const std::string& inviter) {
  if (host_.reach.register_peer_listen && !invite.listen_multiaddrs.empty()) {
    host_.reach.register_peer_listen(inviter, invite.listen_multiaddrs);
  }
  if (!invite.libp2p_peer_id.empty()) {
    host_.reach.note_mesh_peer_id_for_relay(inviter, invite.libp2p_peer_id);
  }
  if (host_.reach.note_caps_for_identity) {
    host_.reach.note_caps_for_identity(inviter, invite.caps, invite.listen_multiaddrs);
  }
  if (host_.reach.note_call_peer_caps && invite.caps.present) {
    host_.reach.note_call_peer_caps(invite.call_id, invite.caps);
  }
  if (host_.reach.prefetch_reach) {
    host_.reach.prefetch_reach(inviter);
  }
}

Roe<void> CallSessionWorkflow::HandleInboundAccept(const std::string& detail_json,
                                                  const std::string& sender_identity,
                                                  const std::string& local_identity) {
  auto accept = CallControlCodec::DecodeAccept(detail_json);
  if (!accept) {
    return accept.error();
  }
  const std::string identity = accept->identity.empty() ? sender_identity : accept->identity;
  log().info << "Inbound CallAccept call_id=" << accept->call_id << " from=" << identity;
  return ApplyRemoteAccept(*accept, identity, local_identity, /*implicit=*/false);
}

Roe<void> CallSessionWorkflow::ApplyImplicitAccept(const std::string& call_id, const std::string& identity,
                                                   const std::string& peer_id, const std::string& local_identity) {
  // B30: the relay can deliver CallAccept tens of seconds late (CN cellular) while the answerer's
  // call-media hello — keyed from the invite — already reached us. Only for a 1:1 call we started
  // whose one remote is still invited: then the hello is the answerer accepting.
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return {};
  }
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants || participants->size() != 2) {
    return {};
  }
  bool local_joined = false;
  bool peer_pending = false;
  for (const CallParticipant& p : *participants) {
    if (p.identity == local_identity) {
      local_joined = p.state == CallParticipantState::Joined;
    } else if (p.identity == identity) {
      peer_pending = p.state == CallParticipantState::Invited || p.state == CallParticipantState::Ringing;
    }
  }
  if (!local_joined || !peer_pending) {
    return {};
  }
  log().info << "Implicit CallAccept (inbound call-media hello before the relay's Accept) call_id=" << call_id
             << " from=" << identity;
  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = identity;
  accept.libp2p_peer_id = peer_id;
  return ApplyRemoteAccept(accept, identity, local_identity, /*implicit=*/true);
}

Roe<void> CallSessionWorkflow::ApplyRemoteAccept(const CallAcceptDetail& accept, const std::string& identity,
                                                 const std::string& local_identity, const bool implicit) {
  NoteAccepterReach(accept, identity, implicit);
  auto session = CommitRemoteJoin(accept, identity, implicit);
  if (!session) {
    return session.error();
  }
  if (session->has_value() && (*session)->state == CallSessionState::Ended) {
    log().info << "Inbound CallAccept ignored (ended session) call_id=" << accept.call_id << " from=" << identity;
    host_.wire.notify_ring_changed();
    return {};
  }
  live_calls_.AddPeer(accept.call_id, identity);
  // Someone answered the call we placed. (Another invitee accepting while we still ring is not us joining.)
  if (const LiveCall* ours = live_calls_.Find(accept.call_id); ours && ours->State() == LiveCallState::Calling) {
    live_calls_.MarkJoined(accept.call_id);
  }
  // B40: register the accept's listen addrs only for a live session. The relay replays old
  // Accepts on every poll; registering those overwrote the DialBook with stale endpoints.
  if (host_.reach.register_peer_listen && !accept.listen_multiaddrs.empty()) {
    host_.reach.register_peer_listen(identity, accept.listen_multiaddrs);
  }
  if (session->has_value()) {
    StartMediaAfterRemoteAccept(accept, identity, local_identity, implicit);
  }
  host_.wire.notify_ring_changed();
  return {};
}

void CallSessionWorkflow::NoteAccepterReach(const CallAcceptDetail& accept, const std::string& identity,
                                            const bool implicit) {
  if (!accept.libp2p_peer_id.empty()) {
    host_.reach.note_mesh_peer_id_for_relay(identity, accept.libp2p_peer_id);
  }
  // An implicit accept carries no caps / listen addrs: keep what the invite path learned.
  if (implicit) {
    return;
  }
  if (host_.reach.note_caps_for_identity) {
    host_.reach.note_caps_for_identity(identity, accept.caps, accept.listen_multiaddrs);
  }
  if (host_.reach.note_call_peer_caps && accept.caps.present) {
    host_.reach.note_call_peer_caps(accept.call_id, accept.caps);
  }
}

Roe<std::optional<CallSession>> CallSessionWorkflow::CommitRemoteJoin(const CallAcceptDetail& accept,
                                                                      const std::string& identity,
                                                                      const bool implicit) {
  CallParticipant participant;
  participant.call_id = accept.call_id;
  participant.identity = identity;
  participant.state = CallParticipantState::Joined;
  participant.media.audio_muted = accept.audio_muted;
  participant.media.video_enabled = accept.video_enabled;
  participant.joined_at = util::NowUnixMs();
  if (auto saved = sessions_.UpsertParticipant(participant); !saved) {
    return saved.error();
  }
  auto loaded = sessions_.LoadSession(accept.call_id);
  std::optional<CallSession> session = loaded ? *loaded : std::nullopt;
  if (session && session->state != CallSessionState::Ended) {
    session->state = CallSessionLogic::TransitionOnRemoteJoined(session->state);
    // video-voice-choice: a 1:1 remote answered voice-only — narrow the caller's session too.
    // Never widens (missing/true field leaves video_allowed untouched); group-thread calls are out
    // of scope for call-wide narrowing (a per-invitee answer only). Keyed on the call's origin, not
    // the live row count, so a mid-call guest cannot leave the two sides disagreeing.
    if (accept.video_allowed && !*accept.video_allowed && session->video_allowed &&
        CallSessionLogic::VoiceAnswerNarrowsCall(*session)) {
      session->video_allowed = false;
      log().info << "call accept voice-only → video disallowed call_id=" << accept.call_id;
    }
    (void)sessions_.UpsertSession(*session);
  }
  // B30: an implicit accept is not the answer yet — its CallAccept (answer mode) is still on the way.
  (void)sessions_.UpdateInviteStatus(accept.call_id, identity, implicit ? "accepted_implicit" : "accepted");
  return session;
}

void CallSessionWorkflow::StartMediaAfterRemoteAccept(const CallAcceptDetail& accept, const std::string& identity,
                                                      const std::string& local_identity, const bool implicit) {
  (void)key_exchange_.SendCurrent(accept.call_id, identity);
  auto joined_after = sessions_.CountJoined(accept.call_id);
  const size_t n_joined = joined_after ? *joined_after : 0;
  if (!implicit && host_.hop.note_accept_hop_report) {
    host_.hop.note_accept_hop_report(accept.call_id, identity, accept.hop_report);
  }
  CallMediaCoordinator* call_media = live_calls_.Media(accept.call_id);
  const bool stays_direct =
      !call_media || call_media->DecideOnRemoteAccept(n_joined, identity) == CallMediaPath::Direct;
  if (stays_direct) {
    live_calls_.SetMediaStatus(accept.call_id, CallMediaStatus::DirectConnecting, "ScheduleStartDirect");
    host_.duplex.schedule_start_direct(accept.call_id, identity, true);
  }
  // Prefetch + roster fan-out as the next event, after media kickoff — avoid starving MediaKey/Connect.
  outbox_.Emit(workflow_event::RosterAfterRemoteAccept{accept.call_id, identity, local_identity});
}

Roe<void> CallSessionWorkflow::HandleInboundDecline(const std::string& detail_json,
                                                   const std::string& sender_identity) {
  auto decline = CallControlCodec::DecodeDecline(detail_json);
  if (!decline) {
    return decline.error();
  }
  const std::string identity = decline->identity.empty() ? sender_identity : decline->identity;
  CallParticipant participant;
  participant.call_id = decline->call_id;
  participant.identity = identity;
  participant.state = CallParticipantState::Declined;
  (void)sessions_.UpsertParticipant(participant);
  (void)sessions_.UpdateInviteStatus(decline->call_id, identity, "declined");

  // CALLS: Decline clears offerer OutboundCalling / sticky Calling bar. End when no remote
  // remains Joined/Ringing/Invited (typical 1:1). Group multi-invitee keeps the call if others
  // still ring. EndCallLocal → RemoteEnded when lifecycle is bound to this call_id.
  auto local = host_.wire.local_relay_identity();
  auto session = sessions_.LoadSession(decline->call_id);
  if (local && session && session->has_value() && (*session)->state != CallSessionState::Ended) {
    bool remote_interest = false;
    if (auto parts = sessions_.ListParticipants(decline->call_id); parts) {
      for (const CallParticipant& row : *parts) {
        if (row.identity == *local) {
          continue;
        }
        if (row.state == CallParticipantState::Joined || row.state == CallParticipantState::Ringing ||
            row.state == CallParticipantState::Invited) {
          remote_interest = true;
          break;
        }
      }
    }
    if (!remote_interest) {
      std::optional<int64_t> duration;
      if ((*session)->created_at > 0) {
        duration = util::NowUnixMs() - (*session)->created_at;
      }
      return EndCallLocal(**session, duration, LiveCallEndReason::DeclinedByPeer);
    }
    live_calls_.RemovePeer(decline->call_id, identity);  // others still in / ringing: the call goes on
  }

  host_.wire.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundLeave(const std::string& detail_json,
                                                 const std::string& sender_identity,
                                                 const std::string& local_identity) {
  auto leave = CallControlCodec::DecodeLeave(detail_json);
  if (!leave) {
    return leave.error();
  }
  const std::string identity = leave->identity.empty() ? sender_identity : leave->identity;
  CallParticipant participant;
  participant.call_id = leave->call_id;
  participant.identity = identity;
  participant.state = CallParticipantState::Left;
  participant.left_at = util::NowUnixMs();
  (void)sessions_.UpsertParticipant(participant);
  auto joined = sessions_.CountJoined(leave->call_id);
  auto session = sessions_.LoadSession(leave->call_id);
  // B24: relay inbox replays old call-control; an ended call's Leave must not re-run EndCallLocal
  // (which stops whatever media a newer call is using).
  if (session && session->has_value() && (*session)->state == CallSessionState::Ended) {
    log().info << "Inbound CallLeave ignored (ended session) call_id=" << leave->call_id
               << " from=" << identity;
    return {};
  }
  if (identity == local_identity) {
    // Ejected after failed soft-migrate (or remote Leave for us): clear local chrome/media.
    host_.hop.clear_sfu_attach_wait();
    if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
      std::optional<int64_t> duration;
      if ((*session)->created_at > 0) {
        duration = util::NowUnixMs() - (*session)->created_at;
      }
      return EndCallLocal(**session, duration, LiveCallEndReason::RemoteEnded);
    }
    live_calls_.StopMedia(leave->call_id);
    host_.wire.notify_ring_changed();
    return {};
  }
  if (session && session->has_value()) {
    const size_t remaining = joined ? *joined : 0;
    const CallSessionState next = CallSessionLogic::TransitionOnLeave((*session)->state, remaining);
    if (next == CallSessionState::Ended) {
      std::optional<int64_t> duration;
      if ((*session)->created_at > 0) {
        duration = util::NowUnixMs() - (*session)->created_at;
      }
      return EndCallLocal(**session, duration, LiveCallEndReason::RemoteEnded);
    }
    live_calls_.RemovePeer(leave->call_id, identity);  // a group call goes on without them
    (*session)->state = next;
    (void)sessions_.UpsertSession(**session);
    (void)MaybeRotateMediaKey(leave->call_id, identity);
  }
  host_.wire.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundRoster(const std::string& detail_json) {
  auto roster = CallControlCodec::DecodeRoster(detail_json);
  if (!roster) {
    return roster.error();
  }
  auto session = sessions_.LoadSession(roster->call_id);
  if (session && session->has_value()) {
    (*session)->media_epoch = roster->media_epoch;
    (void)sessions_.UpsertSession(**session);
  }
  auto local = host_.wire.local_relay_identity ? host_.wire.local_relay_identity() : Roe<std::string>(std::string());
  for (const CallRosterEntry& entry : roster->participants) {
    if (entry.identity.empty()) {
      continue;
    }
    // Our own entry is the sender's (possibly stale, relay-delayed) view of us; only we know our
    // camera/mic, so never let a peer roster overwrite our own state or media (a callee's
    // accept-time roster arriving late turned the caller's just-enabled camera "off" for both
    // sides). The join stamp is the exception: V050 stamps joins on the inviter's clock only and
    // an invitee learns its own from the roster.
    if (local && !local->empty() && entry.identity == *local) {
      if (entry.joined_at) {
        if (auto own = sessions_.FindParticipant(roster->call_id, entry.identity);
            own && own->has_value() && (*own)->joined_at != entry.joined_at) {
          CallParticipant stamped = **own;
          stamped.joined_at = entry.joined_at;
          (void)sessions_.UpsertParticipant(stamped);
        }
      }
      continue;
    }
    // Do not resurrect Left/Declined peers from a stale roster fan-out (blocks re-invite).
    if (entry.state == CallParticipantState::Joined || entry.state == CallParticipantState::Ringing ||
        entry.state == CallParticipantState::Invited) {
      if (auto existing = sessions_.FindParticipant(roster->call_id, entry.identity);
          existing && existing->has_value()) {
        const CallParticipantState prior = (*existing)->state;
        if (prior == CallParticipantState::Left || prior == CallParticipantState::Declined ||
            prior == CallParticipantState::Missed) {
          continue;
        }
        // Joiner BuildRoster still lists earlier invitees as Ringing. Applying that must not
        // demote peers who already Accepted (SoftMigrate N≥3 / in-call UI depend on Joined).
        if (prior == CallParticipantState::Joined &&
            (entry.state == CallParticipantState::Ringing ||
             entry.state == CallParticipantState::Invited)) {
          CallParticipant keep = **existing;
          keep.media.audio_muted = entry.audio_muted;
          keep.media.video_enabled = entry.video_enabled;
          (void)sessions_.UpsertParticipant(keep);
          continue;
        }
      }
    }
    CallParticipant participant;
    participant.call_id = roster->call_id;
    participant.identity = entry.identity;
    participant.state = entry.state;
    participant.media.audio_muted = entry.audio_muted;
    participant.media.video_enabled = entry.video_enabled;
    participant.joined_at = entry.joined_at;
    (void)sessions_.UpsertParticipant(participant);
  }
  // Mid-call invite: CallAccept only reaches the inviter; initiator SoftMigrates when roster
  // shows N≥3 (V021 sticky initiator / V022 payer).
  if (auto joined = sessions_.CountJoined(roster->call_id); joined) {
    host_.hop.on_joined_count_observed(roster->call_id, *joined);
  }
  host_.wire.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundSfuAttach(const std::string& detail_json,
                                                     const std::string& sender_identity) {
  auto attach = CallControlCodec::DecodeSfuAttach(detail_json);
  if (!attach) {
    return attach.error();
  }
  (void)host_.hop.on_inbound_sfu_attach(attach->call_id, *attach, sender_identity);
  host_.wire.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundSfuAttachFailed(const std::string& detail_json,
                                                           const std::string& sender_identity) {
  auto failed = CallControlCodec::DecodeSfuAttachFailed(detail_json);
  if (!failed) {
    return failed.error();
  }
  if (failed->identity.empty()) {
    failed->identity = sender_identity;
  }
  host_.hop.on_inbound_sfu_attach_failed(*failed);
  host_.wire.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundHopRefuse(const std::string& detail_json) {
  auto refused = CallControlCodec::DecodeHopRefuse(detail_json);
  if (!refused) {
    return refused.error();
  }
  host_.hop.on_inbound_hop_refuse(*refused);
  host_.wire.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundVideoRefresh(const std::string& detail_json,
                                                        const std::string& sender_identity) {
  auto refresh = CallControlCodec::DecodeVideoRefresh(detail_json);
  if (!refresh) {
    return refresh.error();
  }
  auto local = host_.wire.local_relay_identity();
  if (!local) {
    return local.error();
  }
  bool sender_joined = false;
  if (auto participants = sessions_.ListParticipants(refresh->call_id); participants) {
    for (const CallParticipant& p : *participants) {
      if (p.identity == sender_identity && p.state == CallParticipantState::Joined) {
        sender_joined = true;
        break;
      }
    }
  }
  if (!CallSessionLogic::ShouldHonorInboundVideoRefresh(refresh->call_id, refresh->identity, sender_identity,
                                                        *local, live_calls_.MediaRunningCallId(), sender_joined)) {
    return {};
  }
  if (CallMediaCoordinator* call_media = live_calls_.Media(refresh->call_id)) {
    call_media->RequestKeyframe();
  }
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundEnded(const std::string& detail_json,
                                                 const std::string& local_identity) {
  auto ended = CallControlCodec::DecodeEnded(detail_json);
  if (!ended) {
    return ended.error();
  }
  (void)sessions_.UpdateInviteStatus(ended->call_id, local_identity, "expired");
  auto session = sessions_.LoadSession(ended->call_id);
  if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
    return EndCallLocal(**session, ended->duration_ms, LiveCallEndReason::RemoteEnded);
  }
  host_.wire.notify_ring_changed();
  return {};
}

} // namespace pbr
