#pragma once

#include "domain/messaging/CallMobility.h"

namespace pbr {

/** What the relay is to a call (K003). */
enum class CallRelayRole {
  /** The relay is the primary path; required, never released during the call. */
  Anchor,
  /** Behind a direct / punched primary: requested for every call, best-effort (a relay may refuse). */
  Standby,
};

/** Order in which a full relay refuses standby reservations (K003): Low goes first. */
enum class CallStandbyPriority {
  Low = 0,
  Medium = 1,
  High = 2,
};

/**
 * Per-call path policy for a 1:1 pair (call-path-resilience M6 / K013). A function of the two
 * mobility classes only — both ends know both from `caps.mobility`, so both compute the same one.
 * Unknown counts as Mobile for relay decisions and as Stationary for punching (K004).
 */
struct CallPathPolicy {
  /** Try a punch while reaching the peer at call start (else: direct dial, then the relay). */
  bool punch_at_start = true;
  /** A relayed call punches for a direct path later and moves onto it (k3 upgrade). */
  bool upgrade_to_direct = true;
  CallRelayRole relay_role = CallRelayRole::Standby;
  /** Standby priority when the call is on a direct (not punched) path; punched is at least Medium. */
  CallStandbyPriority standby_priority = CallStandbyPriority::Low;
  /** Keep a relayed standby next to a direct / punched primary (K003: requested for every call). */
  bool want_relay_standby = true;
};

CallPathPolicy DecideCallPathPolicy(MobilityClass local, MobilityClass remote);

/** Standby priority for the call's current primary path (`punched`: a hole-punched link). */
CallStandbyPriority StandbyPriorityFor(const CallPathPolicy& policy, bool punched);

const char* CallRelayRoleName(CallRelayRole role);
const char* CallStandbyPriorityName(CallStandbyPriority priority);

} // namespace pbr
