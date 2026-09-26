#pragma once

#include "domain/messaging/BroadcastLadderLogic.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Viewer side of the B007 admit-or-redirect ladder for one watch attempt: which hop to ask next
 * and when to attach. Pure (no I/O, no clock); the caller runs the RPCs and feeds outcomes back.
 *
 * - Admit → attach to the admitting hop.
 * - Redirect → ask the hints next (before remaining candidates), within the redirect budget; the
 *   redirecting hop joins the path stamp and is never asked again (loop guard).
 * - Refuse / attach failed → next candidate.
 * - No admission service on the hop (RPC could not run — every relay before B1) → attach to it
 *   directly as a single-hop relay. Admission is capacity management; frames are end-to-end
 *   encrypted (B003), so a relay without admission sees nothing.
 */
class BroadcastViewerLadder {
public:
  enum class Action { Ask, Attach, GiveUp };
  struct Step {
    Action action = Action::GiveUp;
    std::string hop;
    /** GiveUp: why (last refusal / attach failure, or "no hop to ask"). */
    std::string reason;
  };

  explicit BroadcastViewerLadder(std::vector<std::string> candidates,
                                 int redirect_budget = kDefaultBroadcastRedirectBudget, size_t max_asks = 12);

  Step Start();
  Step OnAdmitted(const std::string& hop, const std::string& admitted_hop);
  Step OnRedirect(const std::string& hop, const std::vector<std::string>& redirect_peer_ids,
                  int redirect_budget_remaining);
  Step OnRefused(const std::string& hop, const std::string& reason);
  Step OnNoAdmissionService(const std::string& hop);
  Step OnAttachFailed(const std::string& hop, const std::string& reason);

  /** Request fields for the next ask. */
  int RedirectBudget() const { return redirect_budget_; }
  const std::vector<std::string>& PathStamp() const { return path_stamp_; }

private:
  Step AskNext(const std::string& why);

  std::vector<std::string> queue_;
  std::unordered_set<std::string> asked_;
  std::vector<std::string> path_stamp_;
  int redirect_budget_;
  size_t max_asks_;
  size_t asks_ = 0;
};

} // namespace pbr
