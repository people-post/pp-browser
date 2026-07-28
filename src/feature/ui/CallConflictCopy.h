#pragma once

#include <string>

namespace pbr {

/** Labels + hint for the incoming-call ring when a local call is already active. */
struct CallConflictCopy {
  std::string eyebrow;
  std::string accept_label;
  std::string decline_label;
  /** Empty when not in conflict; otherwise explains that answering ends the current call. */
  std::string hint;
};

/**
 * @param has_conflict True when an ActiveLocalCall exists with a different call_id.
 * @param same_peer True when the inbound inviter is the peer on the active outbound/in-call.
 * @param caller_label Display name (or identity) of the inbound caller.
 * @param active_peer_label Display name of the peer on the call that would be ended.
 */
CallConflictCopy MakeCallConflictCopy(bool has_conflict, bool same_peer, const std::string& caller_label,
                                      const std::string& active_peer_label);

} // namespace pbr
