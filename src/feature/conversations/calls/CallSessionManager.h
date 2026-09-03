#pragma once

#include "foundation/crypto/IPskSessionStore.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallSessionStore.h"
#include "foundation/data/PricingTypes.h"
#include "domain/messaging/InitiationBillingStore.h"
#include "common/thread/IThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/conversations/calls/CallMediaBridge.h"
#include "feature/conversations/calls/CallMediaHost.h"
#include "feature/conversations/calls/CallTopologyController.h"
#include "feature/conversations/MeshMessagingService.h"

#include "common/Module.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Call session lifecycle façade (a2 / V014 / a4).
 * Topology + mesh media live in CallTopologyController / CallMediaBridge.
 */
class CallSessionManager : public Module, private CallTopologyHost, private CallMediaHost {
public:
  using RingChangedFn = std::function<void()>;
  using MediaRelayDeps = CallTopologyController::MediaRelayDeps;

  CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                     CallSessionStore& sessions, CallMediaKeyStore& media_keys, MeshMessagingService& mesh_messaging,
                     IPskSessionStore& psk_store, CallMediaEngine& media);

  void SetOnRingChanged(RingChangedFn callback);
  /** Second listener — mesh (N025 listen) must not overwrite UI chrome refresh. */
  void SetOnRingChangedMesh(RingChangedFn callback);
  using PrefetchPeerReachFn = std::function<void(const std::string& identity)>;
  void SetPrefetchPeerReachability(PrefetchPeerReachFn callback);
  /** Local `/ip4/…/tcp/…/p2p/…` listen set for call-control dial bootstrap. */
  using LocalListenMultiaddrsFn = std::function<std::vector<std::string>()>;
  void SetLocalListenMultiaddrsProvider(LocalListenMultiaddrsFn callback);
  /** Local capability ads for invite/accept (V030). */
  using LocalPeerCapsFn = std::function<CallPeerCaps()>;
  void SetLocalPeerCapsProvider(LocalPeerCapsFn callback);
  /** Local mesh PeerId (base58) for invite/accept — PeerId→relay without contacts. */
  using LocalMeshPeerIdFn = std::function<std::string()>;
  void SetLocalMeshPeerIdProvider(LocalMeshPeerIdFn callback);
  /** Register peer listen multiaddrs from invite/accept into the dial registry. */
  using RegisterPeerListenMultiaddrsFn =
      std::function<void(const std::string& identity, const std::vector<std::string>& multiaddrs)>;
  void SetRegisterPeerListenMultiaddrs(RegisterPeerListenMultiaddrsFn callback);
  /** Cache media_relay ads from invite/accept caps (keyed by mesh PeerId). */
  void NotePeerMediaRelayCap(const std::string& peer_id, bool media_relay);
  /**
   * Remember mesh PeerId ↔ relay: for call-media stream ids (contacts often lack PeerId —
   * PreferLocal dogfood: Moto contact had only relay: so inbound hashed PeerId ≠ SFU stream).
   */
  void NoteMeshPeerIdForRelay(const std::string& relay_identity, const std::string& peer_id);
  bool PeerHasMediaRelayCap(const std::string& peer_id) const;
  std::vector<std::string> ListMediaRelayCapablePeerIds() const;
  void SetMediaRelayDeps(MediaRelayDeps deps);
  void SetCallMediaBridge(CallMediaBridge* bridge);
  /** Optional P001 initiation billing (outbound dial gate + inbound offer check). */
  void SetInitiationBillingStore(InitiationBillingStore* store) { initiation_billing_ = store; }
  InitiationBillingStore* InitiationBilling() const { return initiation_billing_; }
  /** Offer amount stored for inviter when inbound invite carried pricing. */
  int64_t InitiationOfferMinorForPeer(const std::string& peer_identity) const;
  /** Set before AcceptClicked — consumed by AcceptInvite. */
  void SetPendingAcceptChargeDecision(InitiationChargeDecision decision);
  /** Expose private CallMediaHost base for bridge construction (MSVC-safe). */
  CallMediaHost& AsMediaHost() { return *this; }

  Roe<CallSession> StartCall(const std::string& origin_thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);

  Roe<void> AcceptInvite(const std::string& call_id,
                         InitiationChargeDecision charge_decision = InitiationChargeDecision::Waive);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id);
  /** Detach SFU + stop SDL. UI thread only — call before LeaveCall worker / app quit. */
  void StopCallMedia(const std::string& call_id);

  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);

  Roe<std::vector<PendingCallInvite>> ListPendingInvites();
  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();

  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> PeerVideoEnabledForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> VideoAllowedForCall(const std::string& call_id) const;
  Roe<std::vector<CallParticipant>> ListJoinedParticipants(const std::string& call_id) const;

  bool IsAwaitingSfuRecovery() const;
  bool IsSoftMigrateInFlight() const;
  bool IsSfuAttachWaitActive() const;
  bool IsP2pConnectFailed() const;
  bool P2pConnectMissingMic() const;
  Roe<void> RetryP2pMedia(const std::string& call_id);
  void PollP2pConnectHealth();
  void PollPendingSfuAttach();

  std::optional<std::string> TakeLastMediaError();
  /** Latest hop/setup progress line for in-call chrome (empty when idle/connected). */
  std::string PeekMediaActivity() const;
  void ClearMediaActivity();

  Roe<std::optional<CallSession>> SessionForCall(const std::string& call_id) const;
  void SweepExpiredInvites();
  void AbandonOrphanedCallsAfterRestart();
  bool MediaAttemptedThisProcess(const std::string& call_id) const;

  Roe<void> ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity,
                                std::optional<int64_t> relay_created_at_ms = std::nullopt,
                                std::optional<int64_t> relay_server_time_ms = std::nullopt);

  CallMediaEngine& Media();
  /** Combined hop health when SFU attached (empty otherwise). */
  CallHopHealth HopHealth() const;
  bool IsSfuAttached() const;

  Roe<void> SetLocalAudioMuted(bool muted);
  Roe<void> SetLocalVideoEnabled(bool enabled);
  /** Ask publisher for an IDR (empty identity = local encoder). */
  Roe<void> RequestVideoRefresh(const std::string& call_id, const std::string& publisher_identity);

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
  void TopologySetMediaActivity(std::string message) override;
  void TopologyClearMediaActivity() override;
  void TopologyNoteMediaAttempted(const std::string& call_id) override;
  void TopologyBindMediaCallId(const std::string& call_id) override;
  void TopologyClearMediaPeerIdentity() override;
  void TopologyReleaseDirectMedia() override;
  void TopologyRequestInboxSync() override;

  // CallMediaHost
  Roe<std::string> P2pLocalIdentity() const override;
  Roe<void> P2pSendDirect(const std::string& peer_identity, CallControlType type,
                          const std::string& detail_json, const std::string& display) override;
  void P2pNotifyRingChanged() override;
  void P2pSetLastMediaError(std::string message) override;
  Roe<std::optional<std::string>> P2pPeerIdentityForCall(const std::string& call_id) const override;
  Roe<std::optional<std::string>> RelayIdentityForMeshPeerId(const std::string& call_id,
                                                                  const std::string& peer_id) const override;
  bool P2pIsAwaitingSfuRecovery() const override;
  bool P2pExpectGroupSfuMigration(const std::string& call_id) const override;
  void P2pNoteExpectSfuAttach(const std::string& call_id) override;
  bool P2pIsSfuAttached() const override;
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

  // Inbound call-control arms (CallInboundHandlers.cpp) — decode → store → one topology/bridge call.
  Roe<void> HandleInboundInvite(const std::string& detail_json, const std::string& sender_identity,
                                const ThreadMessage& message, std::optional<int64_t> relay_created_at_ms,
                                std::optional<int64_t> relay_server_time_ms, const std::string& local_identity);
  Roe<void> HandleInboundAccept(const std::string& detail_json, const std::string& sender_identity,
                                const std::string& local_identity);
  Roe<void> HandleInboundDecline(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundLeave(const std::string& detail_json, const std::string& sender_identity,
                               const std::string& local_identity);
  Roe<void> HandleInboundRoster(const std::string& detail_json);
  Roe<void> HandleInboundMediaKey(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundSfuAttach(const std::string& detail_json);
  Roe<void> HandleInboundSfuAttachFailed(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundHopRefuse(const std::string& detail_json);
  Roe<void> HandleInboundVideoRefresh(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundEnded(const std::string& detail_json, const std::string& local_identity);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  MeshMessagingService& mesh_messaging_;
  IPskSessionStore& psk_store_;
  CallMediaEngine& media_;
  CallTopologyController topology_;
  CallMediaBridge* call_media_bridge_ = nullptr;
  InitiationBillingStore* initiation_billing_ = nullptr;
  InitiationChargeDecision pending_accept_charge_ = InitiationChargeDecision::Waive;
  bool pending_accept_charge_set_ = false;
  RingChangedFn on_ring_changed_;
  RingChangedFn on_ring_changed_mesh_;
  PrefetchPeerReachFn prefetch_reach_;
  LocalListenMultiaddrsFn local_listen_multiaddrs_;
  LocalPeerCapsFn local_peer_caps_;
  LocalMeshPeerIdFn local_mesh_peer_id_;
  RegisterPeerListenMultiaddrsFn register_peer_listen_multiaddrs_;
  /** PeerId → advertised media_relay (V030). Absent key = unknown / fail closed. */
  std::unordered_map<std::string, bool> peer_media_relay_caps_;
  /** mesh PeerId → relay: identity learned from CallAccept/Invite listen multiaddrs / mDNS. */
  std::unordered_map<std::string, std::string> peer_id_to_relay_;
  std::optional<std::string> last_media_error_;
  std::string media_activity_;
};

} // namespace pbr
