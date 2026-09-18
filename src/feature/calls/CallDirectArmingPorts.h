#pragma once

#include "domain/messaging/CallDirectPlannerLogic.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallLifecycle;

/**
 * Stack-filled Direct arming / outcomes façade for CallMediaBridge (V048 ha3).
 * Speaks Bridge needs only — no CallLifecycle / CallMediaStatus in the port surface.
 * Empty ports = permissive (unit tests), matching former null Lifecycle pointer.
 */
struct CallDirectArmingPorts {
  /** Direct path already armed (DirectConnecting / DirectLive / DegradedTxOnly). */
  std::function<bool()> direct_ops_allowed;
  /**
   * Answerer Schedule when Direct is not armed but the call is still product-active —
   * Stack may re-arm Connecting (phase-gated). No-op if product forbids.
   */
  std::function<void(const std::string& call_id)> request_direct_arming;
  /** Direct-native progress (e.g. DegradedTxOnly); Stack maps to Lifecycle Status. */
  std::function<void(CallDirectPlannerPhase phase, const std::string& call_id)> report_progress;
  std::function<void(const std::string& call_id)> on_connected;
  std::function<void(const std::string& call_id)> on_connect_failed;
  std::function<void(const std::string& call_id)> on_media_deferred;
  std::function<void(const std::string& call_id)> on_media_key_ready;
  std::function<const char*()> arming_debug_name;

  bool IsBound() const { return static_cast<bool>(direct_ops_allowed); }
};

/** Null lifecycle → empty ports (permissive). */
CallDirectArmingPorts MakeCallDirectArmingPorts(CallLifecycle* lifecycle);

} // namespace pbr
