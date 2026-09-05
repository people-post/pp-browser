#include "domain/messaging/AnnounceLiveJoinHandoff.h"

namespace pbr {

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
  if (!plan.hop_peer_id.empty()) {
    handoff.pending.sfu_hint = plan.hop_peer_id;
  }

  handoff.session.call_id = plan.call_id;
  handoff.session.media_mode = handoff.pending.media_mode;
  handoff.session.video_allowed = video_allowed;
  handoff.session.state = CallSessionState::Ringing;
  handoff.session.created_at = now_ms;
  handoff.session.media_epoch = 1;
  if (!plan.hop_peer_id.empty()) {
    handoff.session.sfu_hint = plan.hop_peer_id;
  }
  return handoff;
}

} // namespace pbr
