#include "feature/calls/CallReads.h"

#include "common/Utilities.h"
#include "common/thread/ThreadRecordTypes.h"
#include "lib/calls/CallSessionLogic.h"
#include "lib/calls/CallSessionQueries.h"
#include "common/PbrCompat.h"

namespace pbr {

CallReads::CallReads(const ICallSessionReader& sessions, const ILocalAccount& account, const IThreadCatalog& threads,
                     GroupMembersFn group_members, const IInitiationBillingBook* billing)
    : sessions_(sessions),
      account_(account),
      threads_(threads),
      group_members_(std::move(group_members)),
      billing_(billing) {}

Roe<std::string> CallReads::LocalIdentity() const {
  auto account = account_.GetAccountId();
  if (!account || account->empty()) {
    return Error("Local account identity unavailable");
  }
  return *account;
}

Roe<std::optional<CallSession>> CallReads::ActiveLocalCall() const {
  auto local = LocalIdentity();
  if (!local) {
    return local.error();
  }
  auto active = sessions_.ListActiveSessions();
  if (!active) {
    return active.error();
  }
  for (const CallSession& session : *active) {
    auto participant = sessions_.FindParticipant(session.call_id, *local);
    if (participant && participant->has_value() && (*participant)->state == CallParticipantState::Joined) {
      return std::optional<CallSession>{session};
    }
  }
  return std::optional<CallSession>{};
}

Roe<std::optional<PendingCallInvite>> CallReads::TopPendingInvite() const {
  auto local = LocalIdentity();
  if (!local) {
    return local.error();
  }
  auto pending = sessions_.ListPendingInvitesForInvitee(*local);
  if (!pending) {
    return pending.error();
  }
  const int64_t now = util::NowUnixMs();
  for (const PendingCallInvite& invite : *pending) {
    if (invite.status == "pending" && !CallSessionLogic::IsInviteExpired(invite, now)) {
      return std::optional<PendingCallInvite>{invite};
    }
  }
  return std::optional<PendingCallInvite>{};
}

std::optional<CallSession> CallReads::JoinableCallInThread(const std::string& thread_id) const {
  auto local = LocalIdentity();
  if (!local) {
    return std::nullopt;
  }
  auto thread = threads_.GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Group || !(*thread)->group_id) {
    return std::nullopt;
  }
  if (!InGroup(*(*thread)->group_id, *local)) {
    return std::nullopt;
  }
  return call_queries::JoinableGroupCall(sessions_, *(*thread)->group_id, *local, util::NowUnixMs());
}

bool CallReads::InGroup(const std::string& group_id, const std::string& identity) const {
  if (!group_members_) {
    return true;
  }
  auto members = group_members_(group_id);
  return members && members->count(identity) > 0;
}

Roe<std::optional<std::string>> CallReads::PeerIdentityForCall(const std::string& call_id) const {
  auto local = LocalIdentity();
  if (!local) {
    return local.error();
  }
  auto peer = call_queries::FirstRemoteParticipant(sessions_, call_id, *local);
  if (!peer) {
    return peer.error();
  }
  return *peer ? std::optional<std::string>{(*peer)->identity} : std::optional<std::string>{};
}

Roe<std::optional<bool>> CallReads::PeerVideoEnabledForCall(const std::string& call_id) const {
  auto local = LocalIdentity();
  if (!local) {
    return local.error();
  }
  auto peer = call_queries::FirstRemoteParticipant(sessions_, call_id, *local);
  if (!peer) {
    return peer.error();
  }
  return *peer ? std::optional<bool>{(*peer)->media.video_enabled} : std::optional<bool>{};
}

Roe<std::optional<bool>> CallReads::VideoAllowedForCall(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session) {
    return session.error();
  }
  if (!session->has_value()) {
    return std::optional<bool>{};
  }
  return std::optional<bool>{(*session)->video_allowed};
}

Roe<bool> CallReads::AwaitingExplicitAnswerForCall(const std::string& call_id) const {
  return call_queries::AwaitingExplicitAnswer(sessions_, call_id);
}

Roe<std::vector<CallParticipant>> CallReads::JoinedParticipants(const std::string& call_id) const {
  return call_queries::JoinedParticipants(sessions_, call_id);
}

int64_t CallReads::InitiationOfferMinorForPeer(const std::string& peer_identity) const {
  if (!billing_ || peer_identity.empty()) {
    return 0;
  }
  return billing_->Get(peer_identity).offer_minor;
}

} // namespace pbr
