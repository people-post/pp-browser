#pragma once

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Stack-filled signaling façade for CallLifecycle (V041).
 * Lifecycle must not hold CallSessionManager* — workers copy these functions.
 */
struct CallLifecycleSignalingPorts {
  std::function<Roe<void>(const std::string& call_id)> accept_invite;
  std::function<Roe<void>(const std::string& call_id)> decline_invite;
  std::function<Roe<void>(const std::string& call_id)> leave_call;
  std::function<Roe<void>(const std::string& call_id)> retry_p2p_media;
  std::function<void(const std::string& call_id)> kick_answerer_direct_media;
  /** True when call media engine is active for this call_id. */
  std::function<bool(const std::string& call_id)> media_active_for_call;

  bool IsBound() const { return static_cast<bool>(accept_invite); }
};

} // namespace pbr
