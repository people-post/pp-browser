#include "domain/messaging/BroadcastLadderLogic.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace pbr {
namespace {

bool PathContains(const std::vector<std::string>& path, const std::string& peer_id) {
  if (peer_id.empty()) {
    return false;
  }
  return std::find(path.begin(), path.end(), peer_id) != path.end();
}

std::vector<std::string> PickRedirectChildren(std::vector<std::string> children,
                                              const std::vector<std::string>& path_stamp,
                                              const std::string& self_peer_id, size_t max_hints,
                                              double jitter_unit) {
  std::vector<std::string> eligible;
  eligible.reserve(children.size());
  std::unordered_set<std::string> seen;
  for (std::string& id : children) {
    if (id.empty() || id == self_peer_id || PathContains(path_stamp, id) || seen.count(id) > 0) {
      continue;
    }
    seen.insert(id);
    eligible.push_back(std::move(id));
  }
  if (eligible.empty() || max_hints == 0) {
    return {};
  }

  size_t start = 0;
  if (jitter_unit > 0.0 && eligible.size() > 1) {
    const double unit = std::clamp(jitter_unit, 0.0, 0.999999);
    start = static_cast<size_t>(std::floor(unit * static_cast<double>(eligible.size())));
  }

  std::vector<std::string> out;
  out.reserve(std::min(eligible.size(), max_hints));
  for (size_t i = 0; i < eligible.size() && out.size() < max_hints; ++i) {
    out.push_back(eligible[(start + i) % eligible.size()]);
  }
  return out;
}

} // namespace

BroadcastLadderViewerDecision DecideBroadcastViewerAdmit(const BroadcastLadderViewerInput& in) {
  BroadcastLadderViewerDecision out;
  out.redirect_budget_remaining = in.redirect_budget;

  if (!in.self_peer_id.empty() && PathContains(in.path_stamp, in.self_peer_id)) {
    out.action = BroadcastLadderViewerAction::Refuse;
    out.refuse_reason = "redirect path loop";
    return out;
  }

  if (in.free_viewer_slots > 0) {
    out.action = BroadcastLadderViewerAction::Admit;
    return out;
  }

  // Root / hop overflow: deepen via redirect — do not pin viewers when full (B007).
  if (in.redirect_budget <= 0) {
    out.action = BroadcastLadderViewerAction::Refuse;
    out.refuse_reason = "redirect budget exhausted";
    return out;
  }

  auto redirects = PickRedirectChildren(in.whitelist_online_children, in.path_stamp, in.self_peer_id,
                                        in.max_redirect_hints, in.jitter_unit);
  if (redirects.empty()) {
    out.action = BroadcastLadderViewerAction::Refuse;
    out.refuse_reason = "no online whitelist children";
    return out;
  }

  out.action = BroadcastLadderViewerAction::Redirect;
  out.redirect_peer_ids = std::move(redirects);
  out.redirect_budget_remaining = in.redirect_budget - 1;
  return out;
}

BroadcastLadderSlotWinDecision DecideBroadcastSlotWin(const BroadcastLadderSlotWinInput& in) {
  BroadcastLadderSlotWinDecision out;
  if (in.new_relay_peer_id.empty()) {
    out.action = BroadcastLadderSlotWinAction::Refuse;
    out.refuse_reason = "missing relay peer_id";
    return out;
  }
  if (!in.candidate_on_whitelist) {
    out.action = BroadcastLadderSlotWinAction::Refuse;
    out.refuse_reason = "relay not on parent whitelist";
    return out;
  }
  if (in.slot_win_rate_limited) {
    out.action = BroadcastLadderSlotWinAction::Refuse;
    out.refuse_reason = "slot-win rate limited";
    return out;
  }

  if (in.free_child_slots > 0) {
    out.action = BroadcastLadderSlotWinAction::AdmitRelay;
    return out;
  }

  std::vector<std::string> demote;
  demote.reserve(std::min(in.max_demotions, in.demotable_viewer_peer_ids.size()));
  std::unordered_set<std::string> seen;
  for (const std::string& viewer : in.demotable_viewer_peer_ids) {
    if (viewer.empty() || viewer == in.new_relay_peer_id || seen.count(viewer) > 0) {
      continue;
    }
    seen.insert(viewer);
    demote.push_back(viewer);
    if (demote.size() >= in.max_demotions) {
      break;
    }
  }
  if (demote.empty()) {
    out.action = BroadcastLadderSlotWinAction::Refuse;
    out.refuse_reason = "no demotable viewers";
    return out;
  }

  out.action = BroadcastLadderSlotWinAction::DemoteViewersAndAdmitRelay;
  out.demote_viewer_peer_ids = std::move(demote);
  out.demotion_redirect_target = in.new_relay_peer_id;
  return out;
}

std::vector<std::string> FilterOnlineWhitelist(const std::vector<std::string>& whitelist,
                                               const std::vector<std::string>& online_peer_ids,
                                               size_t max_hints) {
  std::unordered_set<std::string> online;
  online.reserve(online_peer_ids.size());
  for (const std::string& id : online_peer_ids) {
    if (!id.empty()) {
      online.insert(id);
    }
  }
  std::vector<std::string> out;
  out.reserve(std::min(whitelist.size(), max_hints));
  std::unordered_set<std::string> seen;
  for (const std::string& id : whitelist) {
    if (id.empty() || seen.count(id) > 0 || online.count(id) == 0) {
      continue;
    }
    seen.insert(id);
    out.push_back(id);
    if (out.size() >= max_hints) {
      break;
    }
  }
  return out;
}

std::string PrimaryBroadcastHopPeerId(const std::string& hop_peer_id,
                                      const std::vector<std::string>& l1_hop_peer_ids) {
  if (!hop_peer_id.empty()) {
    return hop_peer_id;
  }
  for (const std::string& id : l1_hop_peer_ids) {
    if (!id.empty()) {
      return id;
    }
  }
  return {};
}

} // namespace pbr
