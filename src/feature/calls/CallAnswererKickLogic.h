#pragma once

#include <string>

namespace pbr {

/** Gates for CallSessionManager::KickAnswererDirectMediaIfArmed (V037 / product glue). */
struct CallAnswererKickDecisionInput {
  bool allows_direct_path = false;
  bool media_already_active_same_call = false;
  bool peer_nonempty = false;
};

inline bool ShouldKickAnswererDirectMedia(const CallAnswererKickDecisionInput& in) {
  if (!in.allows_direct_path) {
    return false;
  }
  if (in.media_already_active_same_call) {
    return false;
  }
  return in.peer_nonempty;
}

} // namespace pbr
