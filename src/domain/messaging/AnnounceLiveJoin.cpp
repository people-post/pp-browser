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

} // namespace pbr
