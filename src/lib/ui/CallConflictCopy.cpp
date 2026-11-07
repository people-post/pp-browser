#include "lib/ui/CallConflictCopy.h"

#include "foundation/i18n/LocalizationService.h"

namespace pbr {

namespace {

/** What answering ends, in the words of the call it ends (V064). */
std::string EndingHint(const CallConflictNames& names, const std::string& caller) {
  if (names.same_peer) {
    return Tr("call.ring.conflict_hint_same_peer", {{"caller", caller}});
  }
  if (names.ending_group) {
    return names.ending.empty() ? Tr("call.ring.conflict_hint_group_unnamed")
                                : Tr("call.ring.conflict_hint_group", {{"group", names.ending}});
  }
  return names.ending.empty() ? Tr("call.ring.conflict_hint_unnamed")
                              : Tr("call.ring.conflict_hint", {{"peer", names.ending}});
}

} // namespace

CallConflictCopy MakeCallConflictCopy(const bool has_conflict, const CallConflictNames& names) {
  if (!has_conflict) {
    return {
        .eyebrow = Tr("call.ring.eyebrow"),
        .title = {},
        .accept_label = Tr("call.ring.accept"),
        .decline_label = Tr("call.ring.decline"),
        .hint = {},
    };
  }

  const std::string caller = names.caller.empty() ? Tr("call.label.someone") : names.caller;
  CallConflictCopy copy;
  copy.eyebrow = Tr("call.ring.already_calling");
  copy.title = Tr("call.ring.conflict_title", {{"caller", caller}});
  copy.accept_label = Tr("call.ring.end_and_accept");
  copy.decline_label = Tr("call.ring.ignore");
  copy.hint = EndingHint(names, caller);
  return copy;
}

} // namespace pbr
