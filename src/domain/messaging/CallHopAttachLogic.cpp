#include "domain/messaging/CallHopAttachLogic.h"

#include <unordered_set>

namespace pbr {

CallSfuAttachDetail BuildSfuAttachFanout(const CallSfuAttachDetail& after_local_attach) {
  CallSfuAttachDetail fanout = after_local_attach;
  fanout.quote_id.clear();
  return fanout;
}

uint32_t PublisherStreamIdForIdentity(const std::string& identity) {
  if (identity.empty()) {
    return 1u;
  }
  uint32_t h = 2166136261u;
  for (unsigned char c : identity) {
    h ^= c;
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

SfuAttachWaitPollResult PollSfuAttachWait(const SfuAttachWaitPollInput& in) {
  if (!in.wait_active) {
    return SfuAttachWaitPollResult::Idle;
  }
  if (in.sfu_attached_for_call) {
    return SfuAttachWaitPollResult::ClearAttached;
  }
  // 1:1 P2P may still be connecting — never convert into a group-relay timeout leave.
  if (in.joined_count < 3 && in.media_active_mesh_for_call) {
    return SfuAttachWaitPollResult::ClearAsP2p;
  }
  if (in.soft_migrate_in_flight) {
    return SfuAttachWaitPollResult::Waiting;
  }
  if (in.now_ms < in.deadline_ms) {
    return SfuAttachWaitPollResult::Waiting;
  }
  return SfuAttachWaitPollResult::TimeoutLeave;
}

HopHintOwnerDecision DecideHopHintOwnerAction(const std::vector<std::string>& guest_preferred_peer_ids,
                                              const std::vector<std::string>& owner_ranked_dialable_peer_ids,
                                              const std::string& failed_hop_peer_id) {
  HopHintOwnerDecision out;
  std::unordered_set<std::string> owner_set;
  owner_set.reserve(owner_ranked_dialable_peer_ids.size());
  for (const std::string& id : owner_ranked_dialable_peer_ids) {
    if (!id.empty() && id != failed_hop_peer_id) {
      owner_set.insert(id);
    }
  }
  for (const std::string& pref : guest_preferred_peer_ids) {
    if (pref.empty() || pref == failed_hop_peer_id) {
      continue;
    }
    if (owner_set.count(pref) > 0) {
      out.action = HopHintOwnerAction::RePick;
      out.preferred_hop_peer_id = pref;
      return out;
    }
  }
  out.action = HopHintOwnerAction::RefuseGuest;
  return out;
}

std::vector<std::string> CapGuestHopPreferences(std::vector<std::string> preferred,
                                                const std::string& failed_hop_peer_id,
                                                size_t max_prefs) {
  std::vector<std::string> out;
  out.reserve(std::min(preferred.size(), max_prefs));
  std::unordered_set<std::string> seen;
  for (std::string& id : preferred) {
    if (id.empty() || id == failed_hop_peer_id || seen.count(id) > 0) {
      continue;
    }
    seen.insert(id);
    out.push_back(std::move(id));
    if (out.size() >= max_prefs) {
      break;
    }
  }
  return out;
}

} // namespace pbr
