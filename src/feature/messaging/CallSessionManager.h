#pragma once

#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/P2pMessagingService.h"

#include "common/Module.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Call session lifecycle + pairwise signaling (a1; no WebRTC).
 * Invite-only, hostless, min-identity epoch coordinator (V002/V005/V012).
 */
class CallSessionManager : public Module {
public:
  using RingChangedFn = std::function<void()>;

  CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                     CallSessionStore& sessions, CallMediaKeyStore& media_keys, P2pMessagingService& p2p);

  void SetOnRingChanged(RingChangedFn callback);

  /** Start a 1:1 (or multi-invite) call linked to origin_thread_id. Invites peer identities. */
  Roe<CallSession> StartCall(const std::string& origin_thread_id, CallMediaMode mode,
                             const std::vector<std::string>& invitee_identities);

  Roe<void> AcceptInvite(const std::string& call_id);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id);

  /** Mid-call guest invite (pairwise only; no group membership). */
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);

  Roe<std::vector<PendingCallInvite>> ListPendingInvites();
  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();

  /** Expire stale pending invites; notify UI if any changed. */
  void SweepExpiredInvites();

  /** Apply inbound pairwise call system control (after DM persist). */
  Roe<void> ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity);

private:
  Roe<std::string> LocalRelayIdentity() const;
  Roe<void> SendCallDirectMessage(const std::string& peer_identity, CallControlType type,
                                  const std::string& detail_json, const std::string& display);
  Roe<void> AppendOriginHistory(const std::string& thread_id, CallControlType type, const std::string& text,
                                const std::string& detail_json);
  Roe<void> FanOutToJoined(const std::string& call_id, CallControlType type, const std::string& detail_json,
                           const std::string& display, const std::string& skip_identity);
  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms);
  Roe<CallRosterDetail> BuildRosterDetail(const std::string& call_id) const;
  void NotifyRingChanged();

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  P2pMessagingService& p2p_;
  RingChangedFn on_ring_changed_;
};

} // namespace pbr
