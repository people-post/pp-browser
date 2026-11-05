#pragma once

#include "lib/calls/CallTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The calls this device keeps, read-only (V055): what every part of calls but the session workflow — the
 * rows' one writer — sees of the store.
 */
class ICallSessionReader {
public:
  virtual ~ICallSessionReader() = default;

  virtual Roe<std::optional<CallSession>> LoadSession(const std::string& call_id) const = 0;
  virtual Roe<std::vector<CallSession>> ListActiveSessions() const = 0;
  /** A group's calls in any state, newest first (at most `limit`). */
  virtual Roe<std::vector<CallSession>> ListSessionsForGroup(const std::string& group_id, size_t limit = 20) const = 0;
  /** Every call in any state, newest first (at most `limit`): the phone Home's Calls widget. */
  virtual Roe<std::vector<CallSession>> ListRecentSessions(size_t limit) const = 0;

  virtual Roe<std::vector<CallParticipant>> ListParticipants(const std::string& call_id) const = 0;
  virtual Roe<std::optional<CallParticipant>> FindParticipant(const std::string& call_id,
                                                              const std::string& identity) const = 0;
  virtual Roe<size_t> CountJoined(const std::string& call_id) const = 0;

  virtual Roe<std::optional<PendingCallInvite>> LoadPendingInvite(const std::string& call_id,
                                                                  const std::string& invitee_identity) const = 0;
  virtual Roe<std::vector<PendingCallInvite>> ListPendingInvitesForInvitee(
      const std::string& invitee_identity) const = 0;
};

} // namespace pbr
