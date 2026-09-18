#pragma once

#include "domain/messaging/CallHopPlannerLogic.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallLifecycle;

/**
 * Stack-filled hop arming façade for CallTopologyController / CallHopMigrateWorkflow (V048).
 * Speaks Topology needs only — no CallMediaStatus / Lifecycle types in the port surface.
 * Empty ports = permissive (unit tests), matching former null Lifecycle pointer.
 */
struct CallHopArmingPorts {
  /** Hop path already armed (Waiting / Attaching / Live / Migrating). */
  std::function<bool()> hop_ops_allowed;
  /**
   * SoftMigrate may promote into hop (product still on Direct* / Deciding / None).
   * False when Failed or otherwise blocked — do not report Migrating to force-arm.
   */
  std::function<bool()> soft_migrate_may_arm;
  std::function<uint64_t()> media_cancel_gen;
  /** Hop-native progress; Stack maps to Lifecycle CallMediaStatus. */
  std::function<void(CallHopPlannerPhase phase, const std::string& call_id)> report_progress;
  /** Debug label for logs only (Stack may return Lifecycle Status name). */
  std::function<const char*()> arming_debug_name;

  bool IsBound() const { return static_cast<bool>(hop_ops_allowed); }
};

/** Null lifecycle → empty ports (permissive). */
CallHopArmingPorts MakeCallHopArmingPorts(CallLifecycle* lifecycle);

} // namespace pbr
