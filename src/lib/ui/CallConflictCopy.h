#pragma once

#include <string>

namespace pbr {

/** Labels + hint for the incoming-call ring when a local call is already active. */
struct CallConflictCopy {
  std::string eyebrow;
  /** Empty when not in conflict (the ring says what kind of call it is); otherwise "{caller} is calling". */
  std::string title;
  std::string accept_label;
  std::string decline_label;
  /** Empty when not in conflict; otherwise who answering ends the current call with (V064). */
  std::string hint;
};

/** Who a conflict ring names: the caller, and the call answering would end. */
struct CallConflictNames {
  /** Display name (or identity) of the inbound caller; empty: "Someone". */
  std::string caller;
  /** The call that would end: its peer's display name, or its group's title; empty when not known. */
  std::string ending;
  /** The call that would end is a group's call (`ending` is the group's title). */
  bool ending_group = false;
  /** The inbound caller is the peer on the call that would end. */
  bool same_peer = false;
};

/** @param has_conflict True when an ActiveLocalCall exists with a different call_id. */
CallConflictCopy MakeCallConflictCopy(bool has_conflict, const CallConflictNames& names);

} // namespace pbr
