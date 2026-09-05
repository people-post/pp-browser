#include "domain/messaging/AnnounceLiveJoin.h"

namespace pbr {

bool TipIsLiveJoinable(const PeerAnnounceTip& tip) {
  return tip.state == PeerAnnounceState::Live && !tip.join_handle.empty() && !tip.peer_id.empty();
}

Roe<AnnounceLiveJoinPlan> PlanAnnounceLiveJoin(const PeerAnnounceTip& tip) {
  if (tip.peer_id.empty()) {
    return Error("Missing announce publisher peer_id");
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
  plan.seq = tip.seq;
  plan.epoch = tip.epoch;
  return plan;
}

} // namespace pbr
