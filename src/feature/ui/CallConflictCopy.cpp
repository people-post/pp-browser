#include "feature/ui/CallConflictCopy.h"

#include "foundation/i18n/LocalizationService.h"

namespace pbr {

CallConflictCopy MakeCallConflictCopy(const bool has_conflict, const bool same_peer,
                                      const std::string& caller_label, const std::string& active_peer_label) {
  if (!has_conflict) {
    return {
        .eyebrow = Tr("call.ring.eyebrow"),
        .accept_label = Tr("call.ring.accept"),
        .decline_label = Tr("call.ring.decline"),
        .hint = {},
    };
  }

  CallConflictCopy copy;
  copy.eyebrow = Tr("call.ring.already_calling");
  copy.accept_label = Tr("call.ring.end_and_accept");
  copy.decline_label = Tr("call.ring.ignore");

  const std::string caller = caller_label.empty() ? Tr("call.label.someone") : caller_label;
  const std::string active_peer = active_peer_label.empty() ? Tr("call.label.them") : active_peer_label;
  if (same_peer) {
    copy.hint = Tr("call.ring.callback_hint", {{"caller", caller}});
  } else {
    copy.hint = Tr("call.ring.conflict_hint", {{"peer", active_peer}});
  }
  return copy;
}

} // namespace pbr
