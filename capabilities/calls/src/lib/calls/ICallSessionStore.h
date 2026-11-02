#pragma once

#include "lib/calls/CallTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Where a device keeps its calls (V011, CR024): the sessions, their participants and the invites still to answer.
 * The app's storage keeps them; tests use an in-memory one.
 */
class ICallSessionStore {
public:
  virtual ~ICallSessionStore() = default;

  virtual Roe<void> UpsertSession(const CallSession& session) const = 0;
  virtual Roe<std::optional<CallSession>> LoadSession(const std::string& call_id) const = 0;
  virtual Roe<std::vector<CallSession>> ListActiveSessions() const = 0;
  /** A group's calls in any state, newest first (at most `limit`). */
  virtual Roe<std::vector<CallSession>> ListSessionsForGroup(const std::string& group_id, size_t limit = 20) const = 0;

  virtual Roe<void> UpsertParticipant(const CallParticipant& participant) const = 0;
  virtual Roe<std::vector<CallParticipant>> ListParticipants(const std::string& call_id) const = 0;
  virtual Roe<std::optional<CallParticipant>> FindParticipant(const std::string& call_id,
                                                              const std::string& identity) const = 0;
  virtual Roe<size_t> CountJoined(const std::string& call_id) const = 0;

  virtual Roe<void> UpsertPendingInvite(const PendingCallInvite& invite) const = 0;
  virtual Roe<std::optional<PendingCallInvite>> LoadPendingInvite(const std::string& call_id,
                                                                  const std::string& invitee_identity) const = 0;
  virtual Roe<std::vector<PendingCallInvite>> ListPendingInvitesForInvitee(
      const std::string& invitee_identity) const = 0;
  virtual Roe<void> UpdateInviteStatus(const std::string& call_id, const std::string& invitee_identity,
                                       const std::string& status) const = 0;
};

} // namespace pbr
