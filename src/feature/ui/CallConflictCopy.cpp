#include "feature/ui/CallConflictCopy.h"

namespace pbr {

CallConflictCopy MakeCallConflictCopy(const bool has_conflict, const bool same_peer,
                                      const std::string& caller_label, const std::string& active_peer_label) {
  if (!has_conflict) {
    return {
        .eyebrow = "Incoming call",
        .accept_label = "Accept",
        .decline_label = "Decline",
        .hint = {},
    };
  }

  CallConflictCopy copy;
  copy.eyebrow = "You're already calling";
  copy.accept_label = "End & Accept";
  copy.decline_label = "Ignore";

  const std::string caller = caller_label.empty() ? "Someone" : caller_label;
  const std::string active_peer = active_peer_label.empty() ? "them" : active_peer_label;
  if (same_peer) {
    copy.hint = caller + " is calling you back. Answering ends your outgoing call.";
  } else {
    copy.hint = "Answering will end your current call with " + active_peer + ".";
  }
  return copy;
}

} // namespace pbr
