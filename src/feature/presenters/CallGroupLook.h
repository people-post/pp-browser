#pragma once

#include "lib/calls/CallScreen.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * A call that has been a group call (a group id, or three or more people joined) stays one for the rest of that call:
 * when people leave and two or one remain, the screen keeps the roster, the silent-tile rule and the "alone"
 * countdown instead of flipping to the 1:1 look. Remembered per call id; a different call id starts afresh.
 */
class CallGroupLook {
public:
  /** `group_call`: the call has a group id. `joined`: people joined now. True: show the group look. */
  bool Observe(const std::string& call_id, const bool group_call, const int joined) {
    if (call_id != call_id_) {
      call_id_ = call_id;
      was_group_ = false;
    }
    if (group_call || joined > 2) {
      was_group_ = true;
    }
    return was_group_;
  }

private:
  std::string call_id_;
  bool was_group_ = false;
};

/** The first person other than this device among the people joined now; null when nobody else is in the call. */
inline const CallScreen::Member* FirstJoinedRemote(const std::vector<CallScreen::Member>& members) {
  for (const CallScreen::Member& member : members) {
    if (!member.is_local) {
      return &member;
    }
  }
  return nullptr;
}

} // namespace pbr
