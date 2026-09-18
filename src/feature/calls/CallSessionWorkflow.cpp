#include "feature/calls/CallSessionWorkflow.h"

#include "domain/messaging/CallListenAddrsLogic.h"
#include "domain/messaging/CallMediaPlannerSelectLogic.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/InitiationPricing.h"
#include "domain/messaging/PeerCapsLogic.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

CallSessionWorkflow::CallSessionWorkflow(IThreadStore& store, IdentityStore& identity, CallSessionStore& sessions,
                                         CallMediaKeyStore& media_keys)
    : store_(store), identity_(identity), sessions_(sessions), media_keys_(media_keys) {
  redirectLogger("CallSessionWorkflow");
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
  auto local = host_.local_relay_identity();
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
  auto local = host_.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto pending = sessions_.ListPendingInvitesForInvitee(*local);
  if (!pending) {
    return pending.error();
  }
  std::vector<PendingCallInvite> open;
  for (const PendingCallInvite& invite : *pending) {
    if (invite.status == "pending") {
      open.push_back(invite);
    }
  }
  if (open.empty()) {
    return std::optional<PendingCallInvite>{};
  }
  return std::optional<PendingCallInvite>{open.front()};
}

Roe<void> CallSessionWorkflow::LeaveCallIfActiveExcept(const std::string& keep_call_id) {
  // Drain any conflicting Joined sessions (normally at most one).
  for (int i = 0; i < 4; ++i) {
    auto active = ActiveLocalCall();
    if (!active) {
      return active.error();
    }
    if (!active->has_value() || (*active)->call_id == keep_call_id) {
      return {};
    }
    if (auto left = LeaveCall((*active)->call_id); !left) {
      log().warning << "LeaveCallIfActiveExcept failed for " << (*active)->call_id << ": "
                    << left.error().message;
      return left.error();
    }
  }
  return {};
}

Roe<CallSession> CallSessionWorkflow::StartCall(const std::string& origin_thread_id, const bool video_allowed,
                                               const std::vector<std::string>& invitee_identities) {
  if (invitee_identities.empty()) {
    return Error("At least one invitee required");
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return local.error();
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
  if (initiation_billing_) {
    for (const std::string& invitee : invitee_identities) {
      if (invitee.empty() || invitee == *local || initiation_billing_->IsOpen(invitee)) {
        continue;
      }
      const InitiationPeerBilling billing = initiation_billing_->Get(invitee);
      const int64_t offer = InitiationPricing::DefaultOfferForFloor(billing.floor_minor);
      if (auto payable = InitiationPricing::CheckOutboundPayable(offer); !payable) {
        return payable.error();
      }
    }
  }

  auto key = media_keys_.GenerateEpochKey();
  if (!key) {
    return key.error();
  }
  const std::string call_id = GenerateCallId();
  auto media_key_id = media_keys_.PutEpochKey(call_id, 1, *key);
  if (!media_key_id) {
    return media_key_id.error();
  }

  const int64_t now = util::NowUnixMs();
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = origin_thread_id;
  if ((*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
    session.origin_group_id = *(*thread)->group_id;
  }
  session.media_mode = video_allowed ? CallMediaMode::Video : CallMediaMode::Voice;
  session.video_allowed = video_allowed;
  session.state = CallSessionState::Ringing;
  session.created_at = now;
  session.media_epoch = 1;
  session.media_key_id = *media_key_id;
  if (auto saved = sessions_.UpsertSession(session); !saved) {
    return saved.error();
  }

  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  self.joined_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    return saved.error();
  }

  CallStartedDetail started;
  started.call_id = call_id;
  started.media_mode = session.media_mode;
  started.video_allowed = video_allowed;
  auto started_detail = CallControlCodec::EncodeStarted(started);
  if (!started_detail) {
    return started_detail.error();
  }
  const std::string started_text =
      video_allowed ? "Video call started" : "Voice call started";
  if (auto hist = host_.append_origin_history(origin_thread_id, CallControlType::CallStarted, started_text, *started_detail);
      !hist) {
    return hist.error();
  }

  for (const std::string& invitee : invitee_identities) {
    if (invitee.empty() || invitee == *local) {
      continue;
    }
    if (auto invited = InviteParticipant(call_id, invitee); !invited) {
      return invited.error();
    }
  }

  if (host_.prefetch_reach) {
    for (const auto& _id : invitee_identities) {
      if (!_id.empty()) {
        host_.prefetch_reach(_id);
      }
    }
  }

  host_.notify_ring_changed();
  return session;
}

Roe<void> CallSessionWorkflow::InviteParticipant(const std::string& call_id, const std::string& invitee_identity) {
  auto local = host_.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  if ((*session)->state == CallSessionState::Ended) {
    return Error("Call has ended");
  }
  auto joined = sessions_.CountJoined(call_id);
  if (!joined) {
    return joined.error();
  }
  if (!CallSessionLogic::CanAcceptJoin(*joined)) {
    return Error("Call is full");
  }
  // N≥3 requires media_relay soft-migrate (V021). Refuse mid-call guest invites when no hop
  // exists — otherwise Mac/Linux stay on 1:1 P2P while the invitee hangs on Connecting….
  const bool already_on_sfu =
      (host_.topology_is_on_sfu_for_call && host_.topology_is_on_sfu_for_call(call_id)) ||
      ((*session)->sfu_hint && !(*session)->sfu_hint->empty());
  if (*joined >= 2 && !already_on_sfu &&
      !(host_.topology_has_media_relay_hop_candidates && host_.topology_has_media_relay_hop_candidates())) {
    return Error("Adding a guest needs call hosting help (enable Help host calls on a computer that's helping the network)");
  }

  const int64_t now = util::NowUnixMs();
  PendingCallInvite pending;
  pending.call_id = call_id;
  pending.inviter_identity = *local;
  pending.invitee_identity = invitee_identity;
  pending.media_mode = (*session)->media_mode;
  pending.video_allowed = (*session)->video_allowed;
  pending.origin_thread_id = (*session)->origin_thread_id;
  pending.origin_group_id = (*session)->origin_group_id;
  pending.sfu_hint = (*session)->sfu_hint;
  pending.expires_at = now + kDefaultCallInviteTtlMs;
  pending.created_at = now;
  pending.status = "pending";

  CallInviteDetail invite;
  invite.call_id = call_id;
  invite.inviter_identity = *local;
  invite.invitee_identity = invitee_identity;
  invite.media_mode = (*session)->media_mode;
  invite.video_allowed = (*session)->video_allowed;
  invite.origin_thread_id = (*session)->origin_thread_id;
  invite.origin_group_id = (*session)->origin_group_id;
  invite.sfu_hint = (*session)->sfu_hint;
  invite.expires_at = pending.expires_at;
  if (host_.build_roster_detail) {
    if (auto roster = host_.build_roster_detail(call_id); roster) {
      invite.participants = std::move(roster->participants);
    }
  }
  // Embed epoch key in invite — separate CallMediaKey inbox rows are often ingested without
  // call-control side effects (BenignDuplicate / classifier), so Accept never sees the key.
  invite.media_epoch = (*session)->media_epoch;
  invite.media_key_id = (*session)->media_key_id;
  if (auto key_bytes = media_keys_.LoadEpochKey(call_id, (*session)->media_epoch);
      key_bytes && key_bytes->has_value()) {
    if (host_.resolve_peer_session_key) {
      if (auto session_key = host_.resolve_peer_session_key(invitee_identity)) {
        if (auto wrapped = CallMediaKeyStore::WrapKeyB64(*session_key, **key_bytes, call_id, invite.media_epoch,
                                                         invite.media_key_id)) {
          invite.wrapped_key_b64 = *wrapped;
        } else {
          log().warning << "CallInvite media key wrap failed call_id=" << call_id
                        << " err=" << wrapped.error().message;
        }
      } else {
        log().warning << "CallInvite media key skip; no peer session key peer=" << invitee_identity;
      }
    }
  }
  if (host_.local_listen_multiaddrs) {
    FillCallListenFields(host_.local_listen_multiaddrs(), invite.libp2p_peer_id, invite.listen_multiaddrs);
  }
  // Explicit mesh PeerId wins over /p2p/ suffix derived from listen MAs.
  if (host_.local_mesh_peer_id) {
    if (const std::string pid = host_.local_mesh_peer_id(); !pid.empty()) {
      invite.libp2p_peer_id = pid;
    }
  }
  if (host_.local_peer_caps) {
    invite.caps = host_.local_peer_caps();
    invite.caps.present = true;
  }
  int64_t offer_to_mark = 0;
  int64_t floor_for_mark = 0;
  if (initiation_billing_ && !initiation_billing_->IsOpen(invitee_identity)) {
    const InitiationPeerBilling billing = initiation_billing_->Get(invitee_identity);
    const int64_t offer = InitiationPricing::DefaultOfferForFloor(billing.floor_minor);
    if (auto payable = InitiationPricing::CheckOutboundPayable(offer); !payable) {
      return payable.error();
    }
    invite.offer_amount_minor = offer;
    invite.floor_minor = billing.floor_minor;
    invite.currency = kPricingCurrencyId;
    if (offer > 0) {
      offer_to_mark = offer;
      floor_for_mark = billing.floor_minor;
    }
  }
  auto detail = CallControlCodec::EncodeInvite(invite);
  if (!detail) {
    return detail.error();
  }
  const std::string display =
      (*session)->media_mode == CallMediaMode::Video ? "Incoming video call" : "Incoming voice call";
  if (host_.prefetch_reach) host_.prefetch_reach(invitee_identity);
  // Wire first — do not leave Ringing/pending debris if send fails.
  if (auto sent = host_.send_direct(invitee_identity, CallControlType::CallInvite, *detail, display); !sent) {
    return sent;
  }

  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = invitee_identity;
  participant.state = CallParticipantState::Ringing;
  if (auto saved = sessions_.UpsertParticipant(participant); !saved) {
    return saved.error();
  }
  if (auto saved = sessions_.UpsertPendingInvite(pending); !saved) {
    return saved.error();
  }
  if (initiation_billing_ && offer_to_mark > 0) {
    (void)initiation_billing_->MarkOffered(invitee_identity, offer_to_mark, floor_for_mark);
  }
  log().info << "CallInvite sent call_id=" << call_id << " peer=" << invitee_identity
             << " media_key_embedded=" << (invite.wrapped_key_b64.empty() ? 0 : 1)
             << " listen_addrs=" << invite.listen_multiaddrs.size();
  return {};
}

int64_t CallSessionWorkflow::InitiationOfferMinorForPeer(const std::string& peer_identity) const {
  if (!initiation_billing_ || peer_identity.empty()) {
    return 0;
  }
  return initiation_billing_->Get(peer_identity).offer_minor;
}

void CallSessionWorkflow::SetPendingAcceptChargeDecision(const InitiationChargeDecision decision) {
  pending_accept_charge_ = decision;
  pending_accept_charge_set_ = true;
}

Roe<void> CallSessionWorkflow::AcceptInvite(const std::string& call_id,
                                           InitiationChargeDecision charge_decision) {
  if (pending_accept_charge_set_) {
    charge_decision = pending_accept_charge_;
    pending_accept_charge_set_ = false;
    pending_accept_charge_ = InitiationChargeDecision::Waive;
  }
  log().info << "AcceptInvite start call_id=" << call_id
             << " charge=" << InitiationChargeDecisionToWire(charge_decision);
  auto local = host_.local_relay_identity();
  if (!local) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << local.error().message;
    return local.error();
  }
  SweepExpiredInvites();
  if (auto cleared = LeaveCallIfActiveExcept(call_id); !cleared) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << cleared.error().message;
    return cleared.error();
  }
  // LeaveCallIfActiveExcept only sees Joined sessions. An Ended prior call can leave the
  // engine in sfu_mode (Stop gated on ActiveCallId match) — purge before WaitForAttach.
  // Never Release/Stop the call we are accepting (empty ActiveCallId used to target accept id
  // and PostUIFront-Stop raced answerer StartSfu).
  if ((host_.media_is_active && host_.media_is_active()) || (host_.media_is_sfu_mode && host_.media_is_sfu_mode())) {
    const std::string leftover = (host_.media_active_call_id ? host_.media_active_call_id() : std::string{});
    if (!leftover.empty() && leftover != call_id) {
      log().info << "AcceptInvite stopping leftover media call_id=" << leftover
                 << " accept=" << call_id;
      host_.stop_media_if_call(leftover);
    } else if (leftover.empty()) {
      log().info << "AcceptInvite stopping zombie engine (no ActiveCallId) accept=" << call_id;
      if (host_.stop_media_if_call) {
        host_.stop_media_if_call({});
      } else if (host_.media_stop) {
        host_.media_stop();
      }
    }
  }
  auto pending = sessions_.LoadPendingInvite(call_id, *local);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=Pending call invite not found";
    return Error("Pending call invite not found");
  }
  if (CallSessionLogic::IsInviteExpired(**pending, util::NowUnixMs())) {
    (void)sessions_.UpdateInviteStatus(call_id, *local, "expired");
    host_.notify_ring_changed();
    log().warning << "AcceptInvite end call_id=" << call_id << " err=Call invite expired";
    return Error("Call invite expired");
  }
  if (IsBroadcastSession((*pending)->session_kind)) {
    log().warning << "AcceptInvite end call_id=" << call_id
                  << " err=broadcast sessions use AcceptLiveAnnounceJoin";
    return Error("Broadcast sessions use AcceptLiveAnnounceJoin");
  }

  const std::string inviter = (*pending)->inviter_identity;
  int64_t offer_minor = 0;
  if (initiation_billing_) {
    offer_minor = initiation_billing_->Get(inviter).offer_minor;
  }
  if (charge_decision == InitiationChargeDecision::TakeAll && offer_minor > 0 && !PaymentRailsAvailable()) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=payment_unavailable take_all";
    return Error("payment_unavailable: cannot collect charge yet");
  }

  auto session = sessions_.LoadSession(call_id);
  CallSession row;
  if (session && session->has_value()) {
    row = **session;
  } else {
    row.call_id = call_id;
    row.origin_thread_id = (*pending)->origin_thread_id;
    row.origin_group_id = (*pending)->origin_group_id;
    row.media_mode = (*pending)->media_mode;
    row.video_allowed = (*pending)->video_allowed;
    row.state = CallSessionState::Ringing;
    row.created_at = (*pending)->created_at;
    row.media_epoch = 1;
    row.sfu_hint = (*pending)->sfu_hint;
  }

  auto joined = sessions_.CountJoined(call_id);
  const size_t joined_count = joined ? *joined : 0;
  if (!CallSessionLogic::CanAcceptJoin(joined_count)) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=Call is full";
    return Error("Call is full");
  }

  // Concurrent Accept B → LeaveCallIfActiveExcept may End this call while we are still on the
  // AcceptInvite(A) worker. Do not resurrect an Ended row as Joined.
  if (auto latest = sessions_.LoadSession(call_id); latest && latest->has_value() &&
      (*latest)->state == CallSessionState::Ended) {
    log().info << "AcceptInvite abort call already ended call_id=" << call_id;
    return Error("Call already ended");
  }

  // Build + send CallAccept before Joined / chrome / planner arm — failed send must not leave
  // local Joined or SoftMigrate half-started.
  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = *local;
  accept.video_enabled = false;
  // P001: recipient chooses waive (0) or take_all (rails checked above).
  if (initiation_billing_) {
    accept.offer_amount_minor = offer_minor;
    accept.charge_decision = InitiationChargeDecisionToWire(charge_decision);
  }
  if (host_.local_listen_multiaddrs) {
    FillCallListenFields(host_.local_listen_multiaddrs(), accept.libp2p_peer_id, accept.listen_multiaddrs);
  }
  if (host_.local_mesh_peer_id) {
    if (const std::string pid = host_.local_mesh_peer_id(); !pid.empty()) {
      accept.libp2p_peer_id = pid;
    }
  }
  if (host_.local_peer_caps) {
    accept.caps = host_.local_peer_caps();
    accept.caps.present = true;
  }
  auto detail = CallControlCodec::EncodeAccept(accept);
  if (!detail) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << detail.error().message;
    return detail.error();
  }
  if (auto sent = host_.send_direct(inviter, CallControlType::CallAccept, *detail, "Call accepted"); !sent) {
    log().warning << "CallAccept send failed call_id=" << call_id << " err=" << sent.error().message;
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << sent.error().message;
    return sent.error();
  }
  if (initiation_billing_) {
    (void)initiation_billing_->MarkOpen(inviter);
  }

  const int64_t now = util::NowUnixMs();
  row.state = CallSessionLogic::TransitionOnRemoteJoined(row.state);
  if (auto saved = sessions_.UpsertSession(row); !saved) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << saved.error().message;
    return saved.error();
  }

  CallParticipant self;
  self.call_id = call_id;
  self.identity = *local;
  self.state = CallParticipantState::Joined;
  self.media.video_enabled = false;
  self.joined_at = now;
  if (auto saved = sessions_.UpsertParticipant(self); !saved) {
    log().warning << "AcceptInvite end call_id=" << call_id << " err=" << saved.error().message;
    return saved.error();
  }
  (void)sessions_.UpdateInviteStatus(call_id, *local, "accepted");

  // B-CONFLICT: Accept B may have moved chrome / LeaveCall'd us while CallAccept was on the wire.
  // Do not ScheduleStart or report success for a superseded accept (stale AcceptSucceeded → Idle).
  if (host_.accepting_call_id && host_.active_call_id) {
    const std::string accepting = host_.accepting_call_id();
    const std::string active = host_.active_call_id();
    if ((!accepting.empty() && accepting != call_id) ||
        (accepting.empty() && !active.empty() && active != call_id)) {
      log().info << "AcceptInvite superseded after CallAccept call_id=" << call_id
                 << " accepting=" << accepting << " active=" << active;
      if (auto latest = sessions_.LoadSession(call_id);
          latest && latest->has_value() && (*latest)->state != CallSessionState::Ended) {
        (void)LeaveCall(call_id);
      }
      return Error("Accept superseded");
    }
  }

  // Runs on Accept worker thread (never Browser IO). Do not wait on ListenOn / PollInbox here.
  // ScheduleStart* only posts StartSfu onto UI — never run the engine on this thread.
  // N→planner: Topology for N≥3 / hint; else Bridge Direct (CallMediaPlannerSelectLogic / V038).
  size_t n_joined = 0;
  if (auto joined_after = sessions_.CountJoined(call_id)) {
    n_joined = *joined_after;
  }
  size_t n_active = n_joined;
  if (auto all = sessions_.ListParticipants(call_id); all) {
    n_active = CountMediaPlannerActiveParticipants(*all);
  }
  const size_t planner_n = EffectiveMediaPlannerN(n_joined, n_active);
  const bool topology_took_media =
      host_.on_local_accept_joined && host_.on_local_accept_joined(call_id, planner_n, row.sfu_hint);
  bool schedule_answerer_direct = false;
  if (!topology_took_media) {
    if (row.sfu_hint && !row.sfu_hint->empty()) {
      row.sfu_hint.reset();
      (void)sessions_.UpsertSession(row);
    }
    if (host_.set_direct_connecting) {
      host_.set_direct_connecting(call_id);
    }
    // Drop stale SoftMigrate chrome ("Connecting group media…") from a prior hop attempt.
    if (host_.clear_media_activity) {
      host_.clear_media_activity();
    }
    // Remember peer for Lifecycle KickAnswerer (UI) — PeerIdentityForCall can lag roster.
    pending_answerer_kick_call_id_ = call_id;
    pending_answerer_kick_peer_ = inviter;
    // ScheduleStart after CallAccept is on the wire so offerer can arm inbound while we dial.
    schedule_answerer_direct = true;
  } else {
    pending_answerer_kick_call_id_.clear();
    pending_answerer_kick_peer_.clear();
    log().info << "AcceptInvite topology owns media (no ScheduleStart) call_id=" << call_id
               << " planner_n=" << planner_n
               << " sfu_hint=" << (row.sfu_hint && !row.sfu_hint->empty() ? 1 : 0);
  }
  if (host_.notify_ring_changed) {
    host_.notify_ring_changed();
  }

  if (schedule_answerer_direct) {
    log().info << "AcceptInvite → ScheduleStartDirectMedia (answerer) call_id=" << call_id
               << " inviter=" << inviter << " planner_n=" << planner_n
               << " listen_mas=" << accept.listen_multiaddrs.size()
               << " peer_id=" << (accept.libp2p_peer_id.empty() ? 0 : 1);
    if (host_.schedule_start_direct) {
      host_.schedule_start_direct(call_id, inviter, false);
    }
  }

  // Pull CallMediaKey ASAP — do not wait for the next UI-tick poll (Accept worker path).
  if (host_.sync_inbox_from_wake) {
    host_.sync_inbox_from_wake();
  }

  // Roster / prefetch after Accept returns — keep Accept worker snappy (no Accept hang UX).
  const std::string accept_call_id = call_id;
  const std::string accept_inviter = inviter;
  const std::string accept_local = *local;
  AppRuntime::PostWorkerNormal([this, accept_call_id, accept_inviter, accept_local]() {
    if (!host_.IsBound()) {
      return;
    }
    if (host_.build_roster_detail) {
      if (auto roster = host_.build_roster_detail(accept_call_id); roster) {
        if (auto roster_json = CallControlCodec::EncodeRoster(*roster); roster_json) {
          if (host_.send_direct) {
            (void)host_.send_direct(accept_inviter, CallControlType::CallRoster, *roster_json, "Call roster");
          }
          if (host_.fan_out_joined_and_ringing) {
            (void)host_.fan_out_joined_and_ringing(accept_call_id, CallControlType::CallRoster, *roster_json,
                                                   "Call roster", accept_local);
          }
        }
      }
    }
    if (host_.prefetch_reach) host_.prefetch_reach(accept_inviter);
  });

  log().info << "AcceptInvite end call_id=" << call_id << " ok";
  return {};
}

Roe<void> CallSessionWorkflow::DeclineInvite(const std::string& call_id) {
  auto local = host_.local_relay_identity();
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
  if (!host_.send_direct) {
    return Error("Call session workflow host ports not bound");
  }
  if (auto sent = host_.send_direct((*pending)->inviter_identity, CallControlType::CallDecline, *detail,
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

  // Drop sticky Accept charge if Decline wins the race with a pre-set decision.
  pending_accept_charge_set_ = false;
  pending_accept_charge_ = InitiationChargeDecision::Waive;
  if (pending_answerer_kick_call_id_ == call_id) {
    ClearPendingAnswererKick();
  }

  // CALLS: expire/Decline → Idle chrome + durable Ended (same as Sweep expire path).
  auto session = sessions_.LoadSession(call_id);
  if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
    (void)EndCallLocal(**session, std::nullopt);
  } else if (host_.active_call_id && host_.apply_remote_ended && host_.active_call_id() == call_id) {
    host_.apply_remote_ended(call_id);
  } else if (host_.notify_ring_changed) {
    host_.notify_ring_changed();
  }
  return {};
}

Roe<void> CallSessionWorkflow::MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity) {
  auto local = host_.local_relay_identity();
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

  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  auto key = media_keys_.GenerateEpochKey();
  if (!key) {
    return key.error();
  }
  const uint32_t new_epoch = (*session)->media_epoch + 1;
  auto media_key_id = media_keys_.PutEpochKey(call_id, new_epoch, *key);
  if (!media_key_id) {
    return media_key_id.error();
  }
  (*session)->media_epoch = new_epoch;
  (*session)->media_key_id = *media_key_id;
  if (auto saved = sessions_.UpsertSession(**session); !saved) {
    return saved.error();
  }

  auto roster = host_.build_roster_detail(call_id);
  if (!roster) {
    return roster.error();
  }
  roster->media_epoch = new_epoch;
  auto roster_json = CallControlCodec::EncodeRoster(*roster);
  if (!roster_json) {
    return roster_json.error();
  }
  for (const std::string& peer : remaining) {
    if (peer == *local) {
      continue;
    }
    (void)host_.send_media_key(call_id, peer, new_epoch, *media_key_id, *key);
    (void)host_.send_direct(peer, CallControlType::CallRoster, *roster_json, "Call roster");
  }
  return {};
}

Roe<void> CallSessionWorkflow::EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms) {
  if (host_.stop_media_if_call) {
    host_.stop_media_if_call(session.call_id);
  }
  session.state = CallSessionState::Ended;
  session.ended_at = util::NowUnixMs();
  if (auto saved = sessions_.UpsertSession(session); !saved) {
    return saved.error();
  }
  if (session.origin_thread_id && host_.append_origin_history) {
    CallEndedDetail ended;
    ended.call_id = session.call_id;
    ended.duration_ms = duration_ms;
    auto detail = CallControlCodec::EncodeEnded(ended);
    if (detail) {
      (void)host_.append_origin_history(*session.origin_thread_id, CallControlType::CallEnded, "Call ended", *detail);
    }
  }
  // Product chrome: remote Leave/Ended (and any EndCallLocal for the bound call) must Idle
  // lifecycle without a local LeaveClicked. Skip when lifecycle already moved to another call
  // (e.g. Accept B → LeaveCallIfActiveExcept ends A while Accepting B).
  if (host_.active_call_id && host_.apply_remote_ended &&
      host_.active_call_id() == session.call_id) {
    host_.apply_remote_ended(session.call_id);
  }
  if (host_.notify_ring_changed) {
    host_.notify_ring_changed();
  }
  return {};
}

Roe<void> CallSessionWorkflow::LeaveCall(const std::string& call_id) {
  if (pending_answerer_kick_call_id_ == call_id) {
    ClearPendingAnswererKick();
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return local.error();
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    // Still detach leftover SFU if the disk row is gone but capture is live.
    if (host_.stop_media_if_call) {
      host_.stop_media_if_call(call_id);
    }
    return Error("Call session not found");
  }
  if ((*session)->state == CallSessionState::Ended) {
    // Session already Ended (remote CallEnded / prior EndCallLocal) must still tear down
    // media_relay — otherwise the next Accept inherits zombie RX and red "reconnecting".
    if (host_.stop_media_if_call) {
      host_.stop_media_if_call(call_id);
    }
    return {};
  }
  if (host_.stop_media_if_call) {
    host_.stop_media_if_call(call_id);
  }

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
  if (host_.fan_out_joined) {
    (void)host_.fan_out_joined(call_id, CallControlType::CallLeave, *detail, "Left the call", *local);
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
    if (ended_json && host_.fan_out_joined_and_ringing) {
      // Notify Ringing/Invited invitees as well so offline inbox delivery can clear stale rings.
      (void)host_.fan_out_joined_and_ringing(call_id, CallControlType::CallEnded, *ended_json, "Call ended",
                                             *local);
    }
    return EndCallLocal(**session, duration);
  }

  (*session)->state = next;
  if (auto saved = sessions_.UpsertSession(**session); !saved) {
    return saved.error();
  }
  (void)MaybeRotateMediaKey(call_id, *local);
  if (host_.notify_ring_changed) {
    host_.notify_ring_changed();
  }
  return {};
}

void CallSessionWorkflow::SweepExpiredInvites() {
  auto local = host_.local_relay_identity();
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
        (void)EndCallLocal(**session, std::nullopt);
      } else if (host_.active_call_id && host_.apply_remote_ended &&
                 host_.active_call_id() == invite.call_id) {
        host_.apply_remote_ended(invite.call_id);
      }
      changed = true;
    }
  }

  // CALLS outbound unanswered: clear sticky Calling bar without waiting on GUI LeaveClicked.
  if (host_.is_outbound_calling && host_.is_outbound_calling() &&
      !(host_.media_is_active && host_.media_is_active())) {
    auto active = ActiveLocalCall();
    if (active && active->has_value() &&
        CallSessionLogic::ShouldAutoLeaveOutboundUnanswered(true, false, (*active)->created_at, now)) {
      const std::string call_id = (*active)->call_id;
      if (host_.active_call_id && host_.active_call_id() == call_id) {
        log().warning << "outbound unanswered timeout call_id=" << call_id;
        if (auto left = LeaveCall(call_id); !left) {
          log().warning << "outbound unanswered LeaveCall failed call_id=" << call_id
                        << " err=" << left.error().message;
          auto session = sessions_.LoadSession(call_id);
          if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
            (void)EndCallLocal(**session, std::nullopt);
          }
        }
        return;
      }
    }
  }

  if (changed) {
    host_.notify_ring_changed();
  }
}

void CallSessionWorkflow::AbandonOrphanedCallsAfterRestart() {
  auto local = host_.local_relay_identity();
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
    if (auto left = LeaveCall(call_id); !left) {
      auto session = sessions_.LoadSession(call_id);
      if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
        (void)EndCallLocal(**session, std::nullopt);
      }
    }
  }
  for (CallSession& session : other_live) {
    if (session.state == CallSessionState::Ended) {
      continue;
    }
    (void)EndCallLocal(session, std::nullopt);
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
        (void)EndCallLocal(**session, std::nullopt);
      }
    }
  }

  host_.notify_ring_changed();
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
  // Already joined this call — ignore redelivered / duplicate invites (would demote to Ringing
  // and flicker ring chrome over the in-call banner).
  if (auto existing = sessions_.FindParticipant(invite->call_id, local_identity);
      existing && existing->has_value() && (*existing)->state == CallParticipantState::Joined) {
    log().info << "CallInvite ignored; already joined call_id=" << invite->call_id;
    return {};
  }
  // P001: when we charge (local floor > 0), auto-reject offers below floor.
  if (initiation_billing_) {
    int64_t local_floor = 0;
    if (auto id = identity_.Get()) {
      local_floor = id->initiation_floor;
    }
    if (local_floor > 0) {
      if (auto ok = InitiationPricing::CheckOfferAgainstFloor(invite->offer_amount_minor, local_floor); !ok) {
        log().info << "CallInvite rejected offer_too_low call_id=" << invite->call_id
                   << " offer=" << invite->offer_amount_minor << " floor=" << local_floor;
        CallDeclineDetail decline;
        decline.call_id = invite->call_id;
        decline.identity = local_identity;
        if (auto encoded = CallControlCodec::EncodeDecline(decline)) {
          (void)host_.send_direct(sender_identity, CallControlType::CallDecline, *encoded,
                                      "Call declined (offer too low)");
        }
        return {};
      }
      (void)initiation_billing_->MarkOffered(sender_identity, invite->offer_amount_minor, local_floor);
    }
  }
  const int64_t now = util::NowUnixMs();
  if (CallSessionLogic::ShouldDropStaleInvite(*invite, now, relay_created_at_ms, relay_server_time_ms)) {
    log().warning << "CallInvite dropped as stale call_id=" << invite->call_id
                  << " expires_at=" << (invite->expires_at ? std::to_string(*invite->expires_at) : "none")
                  << " now=" << now
                  << " relay_created=" << (relay_created_at_ms ? std::to_string(*relay_created_at_ms) : "none")
                  << " relay_now=" << (relay_server_time_ms ? std::to_string(*relay_server_time_ms) : "none");
    return {};
  }
  // Near-live invite: re-arm ring TTL from local receipt (skew-safe). Relay-age gate already
  // dropped long-backlogged inbox rows when create/now samples were present.
  if (CallSessionLogic::IsInviteExpired(*invite, now)) {
    log().info << "CallInvite past wire expires_at; re-arming locally call_id=" << invite->call_id
                  << " expires_at=" << (invite->expires_at ? std::to_string(*invite->expires_at) : "none")
                  << " now=" << now;
  }
  PendingCallInvite pending;
  pending.call_id = invite->call_id;
  pending.inviter_identity = invite->inviter_identity.empty() ? sender_identity : invite->inviter_identity;
  // Always key pending rows to local identity so ListPendingInvitesForInvitee matches.
  pending.invitee_identity = local_identity;
  pending.media_mode = invite->media_mode;
  pending.video_allowed = CallSessionLogic::VideoAllowedFromInvite(*invite);
  pending.origin_thread_id = invite->origin_thread_id;
  pending.origin_group_id = invite->origin_group_id;
  pending.sfu_hint = invite->sfu_hint;
  pending.expires_at = now + kDefaultCallInviteTtlMs;
  pending.created_at = message.timestamp > 0 ? message.timestamp : now;
  pending.status = "pending";
  if (auto saved = sessions_.UpsertPendingInvite(pending); !saved) {
    return saved.error();
  }

  CallSession session;
  session.call_id = invite->call_id;
  session.origin_thread_id = invite->origin_thread_id;
  session.origin_group_id = invite->origin_group_id;
  session.media_mode = invite->media_mode;
  session.video_allowed = CallSessionLogic::VideoAllowedFromInvite(*invite);
  session.state = CallSessionState::Ringing;
  session.created_at = pending.created_at;
  session.media_epoch = invite->media_epoch > 0 ? invite->media_epoch : 1;
  session.media_key_id = invite->media_key_id;
  session.sfu_hint = invite->sfu_hint;
  (void)sessions_.UpsertSession(session);

  // Media key embedded in invite (preferred); CallMediaKey message remains a backup.
  if (!invite->wrapped_key_b64.empty()) {
    if (auto session_key = host_.resolve_peer_session_key(sender_identity)) {
      auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(*session_key, invite->wrapped_key_b64, invite->call_id,
                                                       session.media_epoch, invite->media_key_id);
      if (unwrapped) {
        if (auto put = media_keys_.PutEpochKey(invite->call_id, session.media_epoch, *unwrapped); put) {
          log().info << "CallInvite embedded media key stored call_id=" << invite->call_id
                        << " epoch=" << session.media_epoch;
          if (host_.on_media_key_ready) {
            host_.on_media_key_ready(invite->call_id);
          }
        } else {
          log().warning << "CallInvite media key store failed: " << put.error().message;
        }
      } else {
        log().warning << "CallInvite media key unwrap failed: " << unwrapped.error().message;
      }
    } else {
      log().warning << "CallInvite media key missing peer session key from=" << sender_identity;
    }
  }

  // Seed full roster from invite when present; fall back to inviter+self.
  if (!invite->participants.empty()) {
    for (const CallRosterEntry& entry : invite->participants) {
      if (entry.identity.empty()) {
        continue;
      }
      CallParticipant row;
      row.call_id = invite->call_id;
      row.identity = entry.identity;
      row.state = entry.state;
      row.media.audio_muted = entry.audio_muted;
      row.media.video_enabled = entry.video_enabled;
      if (entry.identity == local_identity) {
        row.state = CallParticipantState::Ringing;
      } else if (entry.identity == pending.inviter_identity &&
                 row.state != CallParticipantState::Joined) {
        row.state = CallParticipantState::Joined;
      }
      if (row.state == CallParticipantState::Joined) {
        // Prefer earlier stamp than late acceptors so soft-migrate initiator detection works.
        row.joined_at = session.created_at > 0 ? session.created_at : 1;
      }
      (void)sessions_.UpsertParticipant(row);
    }
  }
  CallParticipant inviter;
  inviter.call_id = invite->call_id;
  inviter.identity = pending.inviter_identity;
  inviter.state = CallParticipantState::Joined;
  inviter.joined_at = session.created_at > 0 ? session.created_at : 1;
  (void)sessions_.UpsertParticipant(inviter);
  CallParticipant self;
  self.call_id = invite->call_id;
  self.identity = local_identity;
  self.state = CallParticipantState::Ringing;
  (void)sessions_.UpsertParticipant(self);
  if (host_.register_peer_listen && !invite->listen_multiaddrs.empty()) {
    host_.register_peer_listen(pending.inviter_identity, invite->listen_multiaddrs);
  }
  if (!invite->libp2p_peer_id.empty()) {
    host_.note_mesh_peer_id_for_relay(pending.inviter_identity, invite->libp2p_peer_id);
  }
  if (host_.note_caps_for_identity) host_.note_caps_for_identity(pending.inviter_identity, invite->caps, invite->listen_multiaddrs);
  if (host_.prefetch_reach) host_.prefetch_reach(pending.inviter_identity);
  host_.notify_ring_changed();
  return {};
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
  if (host_.register_peer_listen && !accept->listen_multiaddrs.empty()) {
    host_.register_peer_listen(identity, accept->listen_multiaddrs);
  }
  if (!accept->libp2p_peer_id.empty()) {
    host_.note_mesh_peer_id_for_relay(identity, accept->libp2p_peer_id);
  }
  if (host_.note_caps_for_identity) host_.note_caps_for_identity(identity, accept->caps, accept->listen_multiaddrs);
  CallParticipant participant;
  participant.call_id = accept->call_id;
  participant.identity = identity;
  participant.state = CallParticipantState::Joined;
  participant.media.audio_muted = accept->audio_muted;
  participant.media.video_enabled = accept->video_enabled;
  participant.joined_at = util::NowUnixMs();
  if (auto saved = sessions_.UpsertParticipant(participant); !saved) {
    return saved.error();
  }
  auto session = sessions_.LoadSession(accept->call_id);
  if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
    (*session)->state = CallSessionLogic::TransitionOnRemoteJoined((*session)->state);
    (void)sessions_.UpsertSession(**session);
  }
  (void)sessions_.UpdateInviteStatus(accept->call_id, identity, "accepted");

  if (session && session->has_value() && (*session)->state == CallSessionState::Ended) {
    log().info << "Inbound CallAccept ignored (ended session) call_id=" << accept->call_id
               << " from=" << identity;
    host_.notify_ring_changed();
    return {};
  }

  if (session && session->has_value()) {
    const uint32_t epoch = (*session)->media_epoch;
    auto key_bytes = media_keys_.LoadEpochKey(accept->call_id, epoch);
    if (key_bytes && key_bytes->has_value()) {
      if (auto keyed = host_.send_media_key(accept->call_id, identity, epoch, (*session)->media_key_id, **key_bytes);
          !keyed) {
        log().warning << "CallMediaKey send failed call_id=" << accept->call_id
                      << " peer=" << identity << " err=" << keyed.error().message;
      } else {
        log().info << "CallMediaKey sent call_id=" << accept->call_id << " peer=" << identity
                      << " epoch=" << epoch;
      }
    } else {
      log().warning << "CallMediaKey missing locally on CallAccept call_id=" << accept->call_id
                    << " epoch=" << epoch;
    }
    auto joined_after = sessions_.CountJoined(accept->call_id);
    const size_t n_joined = joined_after ? *joined_after : 0;
    if (!host_.on_remote_accept_joined(accept->call_id, n_joined, identity)) {
      if (host_.set_direct_connecting) {
        host_.set_direct_connecting(accept->call_id);
      }
      host_.schedule_start_direct(accept->call_id, identity, true);
    }
    // Prefetch + roster fan-out after media kickoff — avoid starving MediaKey/Connect on IO.
    const std::string accept_call_id = accept->call_id;
    const std::string accept_peer = identity;
    AppRuntime::PostWorkerNormal([this, accept_call_id, accept_peer, local = local_identity]() {
      if (host_.prefetch_reach) host_.prefetch_reach(accept_peer);
      if (auto roster = host_.build_roster_detail(accept_call_id); roster) {
        if (auto roster_json = CallControlCodec::EncodeRoster(*roster); roster_json) {
          (void)host_.fan_out_joined_and_ringing(accept_call_id, CallControlType::CallRoster, *roster_json, "Call roster",
                                         local);
        }
      }
    });
  }

  host_.notify_ring_changed();
  return {};
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
  auto local = host_.local_relay_identity();
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
      return EndCallLocal(**session, duration);
    }
  }

  host_.notify_ring_changed();
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
  if (identity == local_identity) {
    // Ejected after failed soft-migrate (or remote Leave for us): clear local chrome/media.
    host_.clear_sfu_attach_wait();
    if (session && session->has_value() && (*session)->state != CallSessionState::Ended) {
      std::optional<int64_t> duration;
      if ((*session)->created_at > 0) {
        duration = util::NowUnixMs() - (*session)->created_at;
      }
      return EndCallLocal(**session, duration);
    }
    host_.stop_media_if_call(leave->call_id);
    host_.notify_ring_changed();
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
      return EndCallLocal(**session, duration);
    }
    (*session)->state = next;
    (void)sessions_.UpsertSession(**session);
    (void)MaybeRotateMediaKey(leave->call_id, identity);
  }
  host_.notify_ring_changed();
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
  for (const CallRosterEntry& entry : roster->participants) {
    if (entry.identity.empty()) {
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
    host_.on_joined_count_observed(roster->call_id, *joined);
  }
  host_.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundMediaKey(const std::string& detail_json,
                                                    const std::string& sender_identity) {
  auto key = CallControlCodec::DecodeMediaKey(detail_json);
  if (!key) {
    return key.error();
  }
  log().info << "Inbound CallMediaKey call_id=" << key->call_id << " epoch=" << key->media_epoch
                << " from=" << sender_identity;
  auto session = sessions_.LoadSession(key->call_id);
  if (session && session->has_value()) {
    (*session)->media_epoch = key->media_epoch;
    (*session)->media_key_id = key->media_key_id;
    (void)sessions_.UpsertSession(**session);
  }
  bool stored = false;
  if (!key->wrapped_key_b64.empty()) {
    auto session_key = host_.resolve_peer_session_key(sender_identity);
    if (session_key) {
      auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(*session_key, key->wrapped_key_b64, key->call_id,
                                                        key->media_epoch, key->media_key_id);
      if (unwrapped) {
        if (auto put = media_keys_.PutEpochKey(key->call_id, key->media_epoch, *unwrapped); put) {
          stored = true;
        } else {
          log().warning << "CallMediaKey store failed: " << put.error().message;
        }
      } else {
        log().warning << "CallMediaKey unwrap failed: " << unwrapped.error().message;
      }
    } else {
      log().warning << "CallMediaKey missing peer session key from=" << sender_identity;
    }
  }
  // Mesh answerer Start waits for epoch key (V015); kick deferred BeginSession.
  if (stored && host_.on_media_key_ready) {
    host_.on_media_key_ready(key->call_id);
  }
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundSfuAttach(const std::string& detail_json) {
  auto attach = CallControlCodec::DecodeSfuAttach(detail_json);
  if (!attach) {
    return attach.error();
  }
  (void)host_.on_inbound_sfu_attach(attach->call_id, *attach);
  host_.notify_ring_changed();
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
  host_.on_inbound_sfu_attach_failed(*failed);
  host_.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundHopRefuse(const std::string& detail_json) {
  auto refused = CallControlCodec::DecodeHopRefuse(detail_json);
  if (!refused) {
    return refused.error();
  }
  host_.on_inbound_hop_refuse(*refused);
  host_.notify_ring_changed();
  return {};
}

Roe<void> CallSessionWorkflow::HandleInboundVideoRefresh(const std::string& detail_json,
                                                        const std::string& sender_identity) {
  auto refresh = CallControlCodec::DecodeVideoRefresh(detail_json);
  if (!refresh) {
    return refresh.error();
  }
  auto local = host_.local_relay_identity();
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
                                                        *local, (host_.media_active_call_id ? host_.media_active_call_id() : std::string{}), sender_joined)) {
    return {};
  }
  if (host_.media_request_keyframe) host_.media_request_keyframe();
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
    return EndCallLocal(**session, ended->duration_ms);
  }
  host_.notify_ring_changed();
  return {};
}

} // namespace pbr
