#pragma once

#include "common/Error.h"
#include "common/directory/ILocalAccount.h"
#include "common/thread/IThreadCatalog.h"
#include "lib/calls/CallGuests.h"
#include "lib/calls/CallTypes.h"
#include "lib/calls/ICallSessionReader.h"
#include "lib/pricing/IInitiationBillingBook.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What the calls' durable state says, read-only (V055): the stored rows and this device's account,
 * never the session manager or its owner state. The screen (CallUiBackend), the hub and the session
 * workflow ask the same questions here. Owned by CallStack beside the stores; the reads are as
 * thread-safe as the stores they read.
 */
class CallReads {
public:
  using GroupMembersFn = std::function<std::optional<std::set<std::string>>(const std::string& group_id)>;

  /** `group_members` empty: membership unknown (every group counts as ours). `billing` may be null. */
  CallReads(const ICallSessionReader& sessions, const ILocalAccount& account, const IThreadCatalog& threads,
            GroupMembersFn group_members, const IInitiationBillingBook* billing,
            CallGroupStandingFn group_standing = {});

  /** This device's account id. */
  Roe<std::string> LocalIdentity() const;
  /** The call this device is joined in, if any. */
  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  /** The newest invite still to answer (expired ones skipped — the owner sweeps them). */
  Roe<std::optional<PendingCallInvite>> TopPendingInvite() const;
  /** V053: the group call this device may join from `thread_id` (a group's thread), if any. */
  std::optional<CallSession> JoinableCallInThread(const std::string& thread_id) const;
  /** `identity` is in `group_id` (true when membership is unknown). */
  bool InGroup(const std::string& group_id, const std::string& identity) const;
  /** V062: `account`'s standing in `group_id`; none when this device holds no roster for it (or it is not checked). */
  std::optional<CallGroupStanding> StandingIn(const std::string& group_id, const std::string& account) const;
  /** V062: guests are checked here (the standing port is bound; tools and tests may leave it unset). */
  bool ChecksGuests() const { return static_cast<bool>(group_standing_); }

  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;
  Roe<std::optional<CallParticipantMedia>> PeerMediaForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> VideoAllowedForCall(const std::string& call_id) const;
  /**
   * True while a remote we invited is joined only through an implicit accept (B30): its CallAccept —
   * and with it a voice-only answer — has not arrived yet.
   */
  Roe<bool> AwaitingExplicitAnswerForCall(const std::string& call_id) const;
  Roe<std::vector<CallParticipant>> JoinedParticipants(const std::string& call_id) const;
  /** Every participant row of `call_id`, in any state (this device's own row too). */
  Roe<std::vector<CallParticipant>> Participants(const std::string& call_id) const;
  /** P001: the initiation offer the inviter `peer_identity` made (0 if none). */
  int64_t InitiationOfferMinorForPeer(const std::string& peer_identity) const;

private:
  const ICallSessionReader& sessions_;
  const ILocalAccount& account_;
  const IThreadCatalog& threads_;
  GroupMembersFn group_members_;
  const IInitiationBillingBook* billing_;
  CallGroupStandingFn group_standing_;
};

} // namespace pbr
