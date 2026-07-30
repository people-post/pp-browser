#pragma once

#include "base/messaging/CallTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Pure call-session helpers (V002 coordinator, invite expiry, state transitions).
 * No I/O — unit-tested in isolation.
 */
class CallSessionLogic {
public:
  /** Min UTF-8 lexicographic communicating identity among joined peers (V002). */
  static std::optional<std::string> SelectEpochCoordinator(const std::vector<std::string>& joined_identities);

  static bool IsInviteExpired(const PendingCallInvite& invite, int64_t now_ms);
  static bool IsInviteExpired(const CallInviteDetail& invite, int64_t now_ms);

  /** Mark pending invites past expires_at as expired/missed. Returns updated rows. */
  static std::vector<PendingCallInvite> ExpirePendingInvites(std::vector<PendingCallInvite> invites,
                                                             int64_t now_ms);

  /**
   * After a remote accept while session is ringing → active.
   * Hostless end: Active ends when fewer than 2 joined remain (empty or sole survivor after peer leave).
   * Ringing stays ringing while at least one joined remains (caller waiting).
   */
  static CallSessionState TransitionOnRemoteJoined(CallSessionState current);
  static CallSessionState TransitionOnLeave(CallSessionState current, size_t remaining_joined);

  static bool CanAcceptJoin(size_t current_joined_count, size_t max_joined = kCallEngineeringMaxJoined);
};

} // namespace pbr
