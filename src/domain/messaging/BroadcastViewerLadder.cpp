#include "domain/messaging/BroadcastViewerLadder.h"

#include <algorithm>
#include <utility>

namespace pbr {

BroadcastViewerLadder::BroadcastViewerLadder(std::vector<std::string> candidates, int redirect_budget,
                                             size_t max_asks)
    : redirect_budget_(redirect_budget), max_asks_(max_asks) {
  for (auto& hop : candidates) {
    if (!hop.empty() && std::find(queue_.begin(), queue_.end(), hop) == queue_.end()) {
      queue_.push_back(std::move(hop));
    }
  }
}

BroadcastViewerLadder::Step BroadcastViewerLadder::AskNext(const std::string& why) {
  while (!queue_.empty()) {
    std::string hop = std::move(queue_.front());
    queue_.erase(queue_.begin());
    if (asked_.count(hop) != 0) {
      continue;
    }
    if (asks_ >= max_asks_) {
      break;
    }
    ++asks_;
    asked_.insert(hop);
    return Step{Action::Ask, std::move(hop), {}};
  }
  return Step{Action::GiveUp, {}, why.empty() ? std::string("no hop to ask") : why};
}

BroadcastViewerLadder::Step BroadcastViewerLadder::Start() {
  return AskNext({});
}

BroadcastViewerLadder::Step BroadcastViewerLadder::OnAdmitted(const std::string& hop,
                                                             const std::string& admitted_hop) {
  return Step{Action::Attach, admitted_hop.empty() ? hop : admitted_hop, {}};
}

BroadcastViewerLadder::Step BroadcastViewerLadder::OnRedirect(const std::string& hop,
                                                             const std::vector<std::string>& redirect_peer_ids,
                                                             int redirect_budget_remaining) {
  path_stamp_.push_back(hop);
  redirect_budget_ = std::min(redirect_budget_ - 1, redirect_budget_remaining);
  if (redirect_budget_ < 0) {
    redirect_budget_ = 0;
    return AskNext("redirect budget exhausted at " + hop);
  }
  // Hints go ahead of the remaining candidates, in the hop's (jittered) order.
  std::vector<std::string> ahead;
  for (const auto& hint : redirect_peer_ids) {
    if (!hint.empty() && asked_.count(hint) == 0 && std::find(ahead.begin(), ahead.end(), hint) == ahead.end()) {
      ahead.push_back(hint);
    }
  }
  queue_.insert(queue_.begin(), ahead.begin(), ahead.end());
  return AskNext("redirected by " + hop + " with no usable hint");
}

BroadcastViewerLadder::Step BroadcastViewerLadder::OnRefused(const std::string& hop, const std::string& reason) {
  return AskNext("refused by " + hop + (reason.empty() ? std::string() : ": " + reason));
}

BroadcastViewerLadder::Step BroadcastViewerLadder::OnNoAdmissionService(const std::string& hop) {
  return Step{Action::Attach, hop, {}};
}

BroadcastViewerLadder::Step BroadcastViewerLadder::OnAttachFailed(const std::string& hop, const std::string& reason) {
  return AskNext("attach to " + hop + " failed: " + reason);
}

} // namespace pbr
