#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/CallLibp2pMediaBridge.h"
#include "feature/messaging/CallP2pSignalingBridge.h"
#include "feature/messaging/CallTopologyController.h"
#include "feature/messaging/P2pMessagingService.h"

#include "common/Module.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Call session lifecycle façade (a2 / V014 / a4).
 * Topology + P2P signaling live in CallTopologyController / CallP2pSignalingBridge.
 */
class CallSessionManager : public Module, private CallTopologyHost, private CallP2pSignalingHost {
public:
  using RingChangedFn = std::function<void()>;
  using MediaRelayDeps = CallTopologyController::MediaRelayDeps;

  CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                     CallSessionStore& sessions, CallMediaKeyStore& media_keys, P2pMessagingService& p2p,
                     IPskSessionStore& psk_store, CallMediaEngine& media);

  void SetOnRingChanged(RingChangedFn callback);
  /** Second listener — mesh (N025 listen) must not overwrite UI chrome refresh. */
  void SetOnRingChangedMesh(RingChangedFn callback);
  using PrefetchPeerReachFn = std::function<void(const std::string& identity)>;
  void SetPrefetchPeerReachability(PrefetchPeerReachFn callback);
  void SetMediaRelayDeps(MediaRelayDeps deps);
  void SetLibp2pMediaBridge(CallLibp2pMediaBridge* bridge);
  /** Expose private CallP2pSignalingHost base for bridge construction (MSVC-safe). */
  CallP2pSignalingHost& AsP2pSignalingHost() { return *this; }

  Roe<CallSession> StartCall(const std::string& origin_thread_id, CallMediaMode mode,
                             const std::vector<std::string>& invitee_identities);

  Roe<void> AcceptInvite(const std::string& call_id);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id);

  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);

  Roe<std::vector<PendingCallInvite>> ListPendingInvites();
  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();

  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> PeerVideoEnabledForCall(const std::string& call_id) const;
  Roe<std::vector<CallParticipant>> ListJoinedParticipants(const std::string& call_id) const;

  bool IsAwaitingSfuRecovery() const;
  bool IsP2pConnectFailed() const;
  bool P2pConnectMissingMic() const;
  Roe<void> RetryP2pMedia(const std::string& call_id);
  void PollP2pConnectHealth();
  void PollPendingSfuAttach();

  Roe<std::optional<CallSession>> SessionForCall(const std::string& call_id) const;
  void SweepExpiredInvites();
  void AbandonOrphanedCallsAfterRestart();
  bool MediaAttemptedThisProcess(const std::string& call_id) const;

  Roe<void> ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity,
                                std::optional<int64_t> relay_created_at_ms = std::nullopt,
                                std::optional<int64_t> relay_server_time_ms = std::nullopt);

  CallMediaEngine& Media();

  Roe<void> SetLocalAudioMuted(bool muted);
  Roe<void> SetLocalVideoEnabled(bool enabled);

  std::optional<std::string> TakeLastMediaError();
  void ClearMediaCallbacks();

private:
  // CallTopologyHost
  Roe<std::string> TopologyLocalIdentity() const override;
  Roe<void> TopologyLeaveCall(const std::string& call_id) override;
  Roe<void> TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                   const std::string& detail_json, const std::string& display,
                                   const std::string& skip_identity) override;
  Roe<void> TopologySendDirect(const std::string& peer_identity, CallControlType type,
                               const std::string& detail_json, const std::string& display) override;
  void TopologyNotifyRingChanged() override;
  void TopologySetLastMediaError(std::string message) override;
  void TopologyNoteMediaAttempted(const std::string& call_id) override;
  void TopologyBindMediaCallId(const std::string& call_id) override;
  void TopologyClearMediaPeerIdentity() override;

  // CallP2pSignalingHost
  Roe<std::string> P2pLocalIdentity() const override;
  Roe<void> P2pSendDirect(const std::string& peer_identity, CallControlType type,
                          const std::string& detail_json, const std::string& display) override;
  void P2pNotifyRingChanged() override;
  void P2pSetLastMediaError(std::string message) override;
  Roe<std::optional<std::string>> P2pPeerIdentityForCall(const std::string& call_id) const override;
  bool P2pIsAwaitingSfuRecovery() const override;
  void P2pOnGroupIceFailed(const std::string& call_id) override;
  void P2pClearAwaitingSfuRecovery() override;
  void P2pResendMediaKey(const std::string& call_id, const std::string& peer_identity) override;
  void P2pRequestInboxSync() override;

  Roe<std::string> LocalRelayIdentity() const;
  Roe<void> SendCallDirectMessage(const std::string& peer_identity, CallControlType type,
                                  const std::string& detail_json, const std::string& display);
  Roe<void> AppendOriginHistory(const std::string& thread_id, CallControlType type, const std::string& text,
                                const std::string& detail_json);
  Roe<void> FanOutToJoined(const std::string& call_id, CallControlType type, const std::string& detail_json,
                           const std::string& display, const std::string& skip_identity);
  /** Fan-out to Joined and Ringing (and Invited) participants — used when ending so invitees clear. */
  Roe<void> FanOutToJoinedAndRinging(const std::string& call_id, CallControlType type,
                                     const std::string& detail_json, const std::string& display,
                                     const std::string& skip_identity);
  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms);
  Roe<CallRosterDetail> BuildRosterDetail(const std::string& call_id) const;
  void NotifyRingChanged();

  Roe<ByteVector> ResolvePeerSessionKey(const std::string& peer_identity) const;
  Roe<void> SendMediaKeyToPeer(const std::string& call_id, const std::string& peer_identity,
                               uint32_t media_epoch, const std::string& media_key_id, const ByteVector& key_bytes);
  void StopMediaIfCall(const std::string& call_id);
  Roe<void> LeaveCallIfActiveExcept(const std::string& keep_call_id);
  void ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity, bool offerer);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  P2pMessagingService& p2p_;
  IPskSessionStore& psk_store_;
  CallMediaEngine& media_;
  CallTopologyController topology_;
  CallP2pSignalingBridge p2p_bridge_;
  CallLibp2pMediaBridge* libp2p_bridge_ = nullptr;
  RingChangedFn on_ring_changed_;
  RingChangedFn on_ring_changed_mesh_;
  PrefetchPeerReachFn prefetch_reach_;
  std::optional<std::string> last_media_error_;
};

} // namespace pbr
