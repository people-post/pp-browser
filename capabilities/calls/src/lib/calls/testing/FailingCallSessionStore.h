#pragma once

// The in-memory call sessions, with reads that fail on demand — what a damaged or busy profile.db gives
// (DEV/PLAN-call-ui-never-detached.md): for the tests of what the calls and the call screen do then. Never linked
// into the app or pp-node.

#include "lib/calls/testing/InMemoryCallStores.h"

#include <atomic>
#include "common/PbrCompat.h"

namespace pbr {

class FailingCallSessionStore : public ICallSessionStore {
public:
  /** ListActiveSessions fails. Any thread. */
  std::atomic<bool> fail_active{false};
  /** ListActiveSessions reads fine and lists no call. Any thread. */
  std::atomic<bool> hide_active{false};
  /** ListParticipants, FindParticipant and CountJoined fail. Any thread. */
  std::atomic<bool> fail_participants{false};

  Roe<void> UpsertSession(const CallSession& session) const override { return rows_.UpsertSession(session); }
  Roe<std::optional<CallSession>> LoadSession(const std::string& call_id) const override {
    return rows_.LoadSession(call_id);
  }
  Roe<std::vector<CallSession>> ListActiveSessions() const override {
    if (fail_active.load()) {
      return Error("ListActiveSessions: failed on purpose (test)");
    }
    if (hide_active.load()) {
      return std::vector<CallSession>{};
    }
    return rows_.ListActiveSessions();
  }
  Roe<std::vector<CallSession>> ListSessionsForGroup(const std::string& group_id, size_t limit = 20) const override {
    return rows_.ListSessionsForGroup(group_id, limit);
  }
  Roe<std::vector<CallSession>> ListRecentSessions(size_t limit) const override {
    return rows_.ListRecentSessions(limit);
  }

  Roe<void> UpsertParticipant(const CallParticipant& participant) const override {
    return rows_.UpsertParticipant(participant);
  }
  Roe<std::vector<CallParticipant>> ListParticipants(const std::string& call_id) const override {
    if (fail_participants.load()) {
      return Error("ListParticipants: failed on purpose (test)");
    }
    return rows_.ListParticipants(call_id);
  }
  Roe<std::optional<CallParticipant>> FindParticipant(const std::string& call_id,
                                                      const std::string& identity) const override {
    if (fail_participants.load()) {
      return Error("ListParticipants: failed on purpose (test)");
    }
    return rows_.FindParticipant(call_id, identity);
  }
  Roe<size_t> CountJoined(const std::string& call_id) const override {
    if (fail_participants.load()) {
      return Error("ListParticipants: failed on purpose (test)");
    }
    return rows_.CountJoined(call_id);
  }

  Roe<void> UpsertPendingInvite(const PendingCallInvite& invite) const override {
    return rows_.UpsertPendingInvite(invite);
  }
  Roe<std::optional<PendingCallInvite>> LoadPendingInvite(const std::string& call_id,
                                                          const std::string& invitee_identity) const override {
    return rows_.LoadPendingInvite(call_id, invitee_identity);
  }
  Roe<std::vector<PendingCallInvite>> ListPendingInvitesForInvitee(const std::string& invitee_identity) const override {
    return rows_.ListPendingInvitesForInvitee(invitee_identity);
  }
  Roe<void> UpdateInviteStatus(const std::string& call_id, const std::string& invitee_identity,
                               const std::string& status) const override {
    return rows_.UpdateInviteStatus(call_id, invitee_identity, status);
  }

private:
  InMemoryCallSessionStore rows_;
};

} // namespace pbr
