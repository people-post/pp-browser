#pragma once

#include "lib/calls/CallTypes.h"
#include "lib/calls/ICallSessionReader.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Where a device keeps its calls (V011, CR024): the sessions, their participants and the invites still to answer.
 * The app's storage keeps them; tests use an in-memory one. Only the session workflow writes (V055); the
 * other parts read through ICallSessionReader.
 */
class ICallSessionStore : public ICallSessionReader {
public:
  virtual Roe<void> UpsertSession(const CallSession& session) const = 0;
  virtual Roe<void> UpsertParticipant(const CallParticipant& participant) const = 0;
  virtual Roe<void> UpsertPendingInvite(const PendingCallInvite& invite) const = 0;
  virtual Roe<void> UpdateInviteStatus(const std::string& call_id, const std::string& invitee_identity,
                                       const std::string& status) const = 0;
};

} // namespace pbr
