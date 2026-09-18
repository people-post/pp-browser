#include "domain/messaging/AnnounceLiveJoin.h"
#include "domain/messaging/BroadcastLadderLogic.h"

namespace pbr {

bool TipIsLiveJoinable(const PeerAnnounceTip& tip) {
  return TipIsProgramKind(tip) && tip.state == PeerAnnounceState::Live && !tip.join_handle.empty() &&
         !tip.peer_id.empty();
}

Roe<AnnounceLiveJoinPlan> PlanAnnounceLiveJoin(const PeerAnnounceTip& tip) {
  if (tip.peer_id.empty()) {
    return Error("Missing announce publisher peer_id");
  }
  if (!TipIsProgramKind(tip)) {
    return Error("live_chat tips are not joinable");
  }
  if (tip.state != PeerAnnounceState::Live) {
    return Error("Announce tip is not live");
  }
  if (tip.join_handle.empty()) {
    return Error("Live announce tip missing join_handle");
  }

  AnnounceLiveJoinPlan plan;
  plan.call_id = tip.join_handle;
  plan.publisher_peer_id = tip.peer_id;
  plan.topic_id = tip.topic_id;
  plan.program_id = tip.program_id;
  plan.l1_hop_peer_ids = tip.l1_hop_peer_ids;
  plan.hop_peer_id = PrimaryBroadcastHopPeerId(tip.hop_peer_id, tip.l1_hop_peer_ids);
  plan.seq = tip.seq;
  plan.epoch = tip.epoch;
  return plan;
}

Roe<AnnounceLiveJoinHandoff> BuildAnnounceLiveJoinHandoff(const AnnounceLiveJoinPlan& plan,
                                                          const std::string_view local_invitee_identity,
                                                          const std::string_view inviter_identity,
                                                          const int64_t now_ms, const bool video_allowed) {
  if (plan.call_id.empty()) {
    return Error("Live-join plan missing call_id");
  }
  if (local_invitee_identity.empty()) {
    return Error("Missing local invitee identity");
  }
  if (inviter_identity.empty()) {
    return Error("Missing announce inviter identity");
  }

  AnnounceLiveJoinHandoff handoff;
  handoff.pending.call_id = plan.call_id;
  handoff.pending.inviter_identity = std::string(inviter_identity);
  handoff.pending.invitee_identity = std::string(local_invitee_identity);
  handoff.pending.media_mode = video_allowed ? CallMediaMode::Video : CallMediaMode::Voice;
  handoff.pending.video_allowed = video_allowed;
  handoff.pending.created_at = now_ms;
  handoff.pending.expires_at = now_ms + kDefaultCallInviteTtlMs;
  handoff.pending.status = "pending";
  handoff.pending.session_kind = CallSessionKind::Broadcast;
  if (!plan.hop_peer_id.empty()) {
    handoff.pending.sfu_hint = plan.hop_peer_id;
  }

  handoff.session.call_id = plan.call_id;
  handoff.session.media_mode = handoff.pending.media_mode;
  handoff.session.video_allowed = video_allowed;
  handoff.session.state = CallSessionState::Ringing;
  handoff.session.created_at = now_ms;
  handoff.session.media_epoch = plan.media_epoch == 0 ? 1 : plan.media_epoch;
  handoff.session.media_key_id = plan.media_key_id;
  handoff.session.session_kind = CallSessionKind::Broadcast;
  if (!plan.hop_peer_id.empty()) {
    handoff.session.sfu_hint = plan.hop_peer_id;
  }
  return handoff;
}

} // namespace pbr
