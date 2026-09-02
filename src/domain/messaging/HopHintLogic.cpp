#include "domain/messaging/HopHintLogic.h"

#include <unordered_set>

namespace pbr {

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
