#pragma once

#include "domain/messaging/CallTypes.h"

#include <cstddef>
#include <vector>

namespace pbr {

/**
 * Pure N→planner selection (V037/V038).
 * N=2 → Bridge (direct/punch/circuit); N≥3 → Topology (SoftMigrate / media_relay).
 * CallSessionManager must not re-encode these trees on every Accept path.
 *
 * Threshold matches `CallMediaTopology::ShouldUseMediaRelay` (joined_count >= 3) without
 * taking a domain/media peer edge.
 */

/** Joined + Ringing + Invited count toward SoftMigrate / WaitForAttach (V021/V038). */
inline bool IsMediaPlannerActiveParticipant(CallParticipantState state) {
  return state == CallParticipantState::Joined || state == CallParticipantState::Ringing ||
         state == CallParticipantState::Invited;
}

inline size_t CountMediaPlannerActiveParticipants(const std::vector<CallParticipant>& rows) {
  size_t n = 0;
  for (const CallParticipant& p : rows) {
    if (IsMediaPlannerActiveParticipant(p.state)) {
      ++n;
    }
  }
  return n;
}

/** Prefer roster active count when it exceeds CountJoined (other callees still Ringing). */
inline size_t EffectiveMediaPlannerN(size_t joined_count, size_t active_roster_count) {
  return active_roster_count > joined_count ? active_roster_count : joined_count;
}

inline bool ShouldArmHopPlanner(size_t effective_n) {
  return effective_n >= 3;
}

struct CallExpectGroupSfuInput {
  size_t joined_count = 0;
  size_t active_roster_count = 0;
  bool has_sfu_hint = false;
  bool sfu_attached = false;
  bool awaiting_sfu_recovery = false;
};

/** Bridge SoftMigrate EOF / chrome: expect hop path for this call. */
inline bool ShouldExpectGroupSfuMigration(const CallExpectGroupSfuInput& in) {
  if (in.awaiting_sfu_recovery || in.sfu_attached) {
    return true;
  }
  if (ShouldArmHopPlanner(EffectiveMediaPlannerN(in.joined_count, in.active_roster_count))) {
    return true;
  }
  return in.has_sfu_hint;
}

struct CallRelayCapNudgeInput {
  bool media_relay_newly_true = false;
  size_t effective_n = 0;
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
  if (ShouldArmHopPlanner(in.effective_n)) {
    return true;
  }
  return in.sfu_attach_wait_active;
}

} // namespace pbr
