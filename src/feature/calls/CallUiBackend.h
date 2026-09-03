#pragma once

#include "foundation/data/PricingTypes.h"
#include "common/media/CallMediaHealth.h"
#include "domain/messaging/CallTypes.h"
#include "common/Error.h"
#include "feature/calls/CallLifecycle.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class CallMediaEngine;
class CallStack;

/**
 * Sealed UI-facing façade over the CallStack session + lifecycle.
 * Queries stack.Calls()/Lifecycle() per call so stack rebuilds stay transparent.
 * Application owns the instance (bound to ConversationsHub::CallStackRef());
 * CallController binds via CallFunctionalPorts.
 */
class CallUiBackend {
public:
  explicit CallUiBackend(CallStack& stack);

  bool Available() const;
  /** Stable identity for rebind detection (CallSessionManager* as opaque). */
  const void* SessionsIdentity() const;

  void SetOnRingChanged(std::function<void()> callback);
  void SetOnChromeRefresh(std::function<void()> callback);

  void SweepExpiredInvites();
  void PollPendingSfuAttach();
  void PollP2pConnectHealth();

  std::optional<std::string> TakeLastMediaError();
  std::string PeekMediaActivity() const;
  void ClearMediaActivity();

  Roe<std::optional<PendingCallInvite>> TopPendingInvite();
  Roe<std::optional<CallSession>> ActiveLocalCall();
  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> PeerVideoEnabledForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> VideoAllowedForCall(const std::string& call_id) const;
  Roe<std::vector<CallParticipant>> ListJoinedParticipants(const std::string& call_id) const;

  bool IsAwaitingSfuRecovery() const;
  bool IsSoftMigrateInFlight() const;
  bool IsSfuAttachWaitActive() const;
  bool IsP2pConnectFailed() const;
  bool P2pConnectMissingMic() const;
  bool MediaAttemptedThisProcess(const std::string& call_id) const;

  Roe<void> LeaveCall(const std::string& call_id);
  Roe<CallSession> StartCall(const std::string& origin_thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);
  void StopCallMedia(const std::string& call_id);

  Roe<void> SetLocalAudioMuted(bool muted);
  Roe<void> SetLocalVideoEnabled(bool enabled);
  Roe<void> RequestVideoRefresh(const std::string& call_id, const std::string& publisher_identity);

  /** Requires Available(); CallController still needs tiles/levels via CallMediaEngine. */
  CallMediaEngine& Media();
  CallHopHealth HopHealth() const;

  // Lifecycle
  const std::string& LastError() const;
  void ClearLastError();
  bool ShouldSuppressRing(const std::string& call_id) const;
  CallPhase Phase() const;
  void Apply(CallLifecycleEvent ev, const std::string& call_id = {});
  void NoteRingCallId(const std::string& call_id);
  const std::string& LastRingCallId() const;
  const std::string& ActiveCallId() const;

  /** P001 initiation offer stored for inbound inviter (0 if none). */
  int64_t InitiationOfferMinorForPeer(const std::string& peer_identity) const;
  /** Set before AcceptClicked — consumed by AcceptInvite. */
  void SetPendingAcceptChargeDecision(InitiationChargeDecision decision);

private:
  CallStack& stack_;
};

} // namespace pbr
