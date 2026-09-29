#pragma once

#include "domain/messaging/CallTypes.h"

#include <cstddef>

namespace pbr {

/**
 * Pure N→planner selection (V037/V038/V050).
 * N=2 → Bridge (direct/punch/circuit); N≥3 → Topology (SoftMigrate / media_relay).
 * CallSessionManager must not re-encode these trees on every Accept path.
 *
 * N is the **joined** count on every side (V050): ringing / invited participants never arm the
 * hop planner — the initiator counts joined when an accept lands, and an invitee's roster may be
 * a snapshot, so counting ringing rows made the first acceptor's path depend on invite order.
 *
 * Threshold matches `CallMediaTopology::ShouldUseMediaRelay` (joined_count >= 3) without
 * taking a domain/media peer edge.
 */
inline bool ShouldArmHopPlanner(size_t joined_count) {
  return joined_count >= 3;
}

struct CallExpectGroupSfuInput {
  size_t joined_count = 0;
  bool has_sfu_hint = false;
  bool sfu_attached = false;
  bool awaiting_sfu_recovery = false;
};

/** Bridge SoftMigrate EOF / chrome: expect hop path for this call. */
inline bool ShouldExpectGroupSfuMigration(const CallExpectGroupSfuInput& in) {
  if (in.awaiting_sfu_recovery || in.sfu_attached) {
    return true;
  }
  if (ShouldArmHopPlanner(in.joined_count)) {
    return true;
  }
  return in.has_sfu_hint;
}

struct CallRelayCapNudgeInput {
  bool media_relay_newly_true = false;
  size_t joined_count = 0;
  bool sfu_attach_wait_active = false;
  bool sfu_attached = false;
  bool already_on_sfu_for_call = false;
};

/**
 * After learning media_relay=true for a peer: SoftMigrate re-pick only for N≥3 / attach-wait.
 * Plain 1:1 must stay Direct (dogfood: PreferLocal SoftMigrate while answerer DirectConnecting).
 */
inline bool ShouldNudgeSoftMigrateOnRelayCap(const CallRelayCapNudgeInput& in) {
  if (!in.media_relay_newly_true) {
    return false;
  }
  if (in.already_on_sfu_for_call) {
    return false;
  }
  if (in.sfu_attached && !in.sfu_attach_wait_active) {
    return false;
  }
  if (ShouldArmHopPlanner(in.joined_count)) {
    return true;
  }
  return in.sfu_attach_wait_active;
}

} // namespace pbr
