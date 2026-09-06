#include "feature/calls/BroadcastSessionCoordinator.h"

#include "domain/messaging/AnnounceLiveJoinHandoff.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/CallSessionLogic.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/people/ContactIdentity.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/ContactTypes.h"
#include "common/Utilities.h"

#include "common/PbrCompat.h"

namespace pbr {

BroadcastSessionCoordinator::BroadcastSessionCoordinator(CallSessionStore& sessions,
                                                         ContactsStore& contacts,
                                                         CallMediaKeyStore& media_keys)
    : sessions_(sessions), contacts_(contacts), media_keys_(media_keys) {
  redirectLogger("BroadcastSessionCoordinator");
}

void BroadcastSessionCoordinator::SetHostPorts(HostPorts ports) {
  host_ = std::move(ports);
}

Roe<PendingCallInvite> BroadcastSessionCoordinator::ArmJoinFromLiveAnnounce(const AnnounceLiveJoinPlan& plan,
                                                                   const ArmLiveAnnounceJoinOpts& opts) {
  if (!host_.local_relay_identity || !host_.notify_ring_changed) {
    return Error("Broadcast session coordinator host ports not bound");
  }
  if (plan.call_id.empty()) {
    return Error("Live-join plan missing call_id");
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return local.error();
  }

  std::string inviter = plan.publisher_peer_id;
  if (auto found = contacts_.FindByIdentity(plan.publisher_peer_id, ContactIdKind::PeerId)) {
    if (*found) {
      const std::string account = PrimaryIdOfKind(**found, ContactIdKind::Account);
      if (!account.empty()) {
        inviter = account;
      }
    }
  }

  AnnounceLiveJoinPlan effective = plan;
  if (opts.ticket != nullptr) {
    if (opts.publisher_mldsa_public_key == nullptr || opts.publisher_mldsa_public_key->empty()) {
      return Error("Live-announce ticket apply requires publisher ML-DSA public key");
    }
    const int64_t now_ms = opts.now_ms != 0 ? opts.now_ms : util::NowUnixMs();
    std::string viewer_peer_id;
    if (host_.local_mesh_peer_id) {
      viewer_peer_id = host_.local_mesh_peer_id();
    }
    if (viewer_peer_id.empty()) {
      viewer_peer_id = opts.ticket->viewer_peer_id;
    }
    auto applied = ApplyBroadcastJoinTicket(media_keys_, *opts.ticket, *opts.publisher_mldsa_public_key,
                                            now_ms, viewer_peer_id, opts.viewer_pairwise_session_key);
    if (!applied) {
      return applied.error();
    }
    effective.media_epoch = applied->media_epoch;
    effective.media_key_id = applied->media_key_id;
    if (effective.hop_peer_id.empty() && !opts.ticket->hop_peer_id.empty()) {
      effective.hop_peer_id = opts.ticket->hop_peer_id;
    }
    log().info << "ArmJoinFromLiveAnnounce applied ticket call_id=" << effective.call_id
               << " media_epoch=" << effective.media_epoch << " media_key_id=" << effective.media_key_id;
  }

  auto handoff = BuildAnnounceLiveJoinHandoff(effective, *local, inviter, util::NowUnixMs(), true);
  if (!handoff) {
    return handoff.error();
  }

  if (auto saved = sessions_.UpsertPendingInvite(handoff->pending); !saved) {
    return saved.error();
  }
  if (auto saved = sessions_.UpsertSession(handoff->session); !saved) {
    return saved.error();
  }

  // Seed inviter as ringing so AcceptInvite / roster paths see a peer.
  CallParticipant inviter_row;
  inviter_row.call_id = plan.call_id;
  inviter_row.identity = inviter;
  inviter_row.state = CallParticipantState::Ringing;
  inviter_row.joined_at = handoff->pending.created_at;
  (void)sessions_.UpsertParticipant(inviter_row);

  host_.notify_ring_changed();
  log().info << "ArmJoinFromLiveAnnounce call_id=" << plan.call_id << " inviter=" << inviter
             << " topic=" << plan.topic_id << " program=" << plan.program_id;
  return handoff->pending;
}

Roe<void> BroadcastSessionCoordinator::AcceptLiveAnnounceJoin(const std::string& call_id) {
  log().info << "AcceptLiveAnnounceJoin start call_id=" << call_id;
  if (!host_.local_relay_identity || !host_.notify_ring_changed || !host_.sweep_expired_invites ||
      !host_.leave_call_if_active_except || !host_.on_announce_viewer_joined) {
    return Error("Broadcast session coordinator host ports not bound");
  }
  auto local = host_.local_relay_identity();
  if (!local) {
    return local.error();
  }
  host_.sweep_expired_invites();
  if (auto cleared = host_.leave_call_if_active_except(call_id); !cleared) {
    return cleared.error();
  }
  auto pending = sessions_.LoadPendingInvite(call_id, *local);
  if (!pending || !pending->has_value() || (*pending)->status != "pending") {
    return Error("Pending live-announce invite not found");
  }
  if (CallSessionLogic::IsInviteExpired(**pending, util::NowUnixMs())) {
    (void)sessions_.UpdateInviteStatus(call_id, *local, "expired");
    host_.notify_ring_changed();
    return Error("Live-announce invite expired");
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
    row.session_kind = (*pending)->session_kind;
  }
  if (!row.sfu_hint && (*pending)->sfu_hint) {
    row.sfu_hint = (*pending)->sfu_hint;
  }
  if (!IsBroadcastSession(row.session_kind)) {
    row.session_kind = CallSessionKind::Broadcast;
  }

  auto joined = sessions_.CountJoined(call_id);
  const size_t joined_count = joined ? *joined : 0;
  if (!CallSessionLogic::CanAcceptJoin(joined_count)) {
    return Error("Call is full");
  }

  const int64_t now = util::NowUnixMs();
  row.state = CallSessionLogic::TransitionOnRemoteJoined(row.state);
  if (auto saved = sessions_.UpsertSession(row); !saved) {
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
  (void)sessions_.UpdateInviteStatus(call_id, *local, "accepted");

  // No SoftMigrate, no 1:1 ScheduleStartDirectMedia, no CallAccept wire to publisher.
  const bool sfu_scheduled = host_.on_announce_viewer_joined(call_id, row.sfu_hint);
  host_.notify_ring_changed();
  log().info << "AcceptLiveAnnounceJoin end call_id=" << call_id
             << " sfu_scheduled=" << (sfu_scheduled ? 1 : 0) << " ok";
  return {};
}

} // namespace pbr
