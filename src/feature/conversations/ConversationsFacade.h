#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "domain/messaging/AtAiParser.h"
#include "domain/messaging/GroupTypes.h"
#include "common/thread/IThreadStore.h"
#include "domain/messaging/SendRelayOptions.h"
#include "common/thread/SyncStateTypes.h"
#include "common/thread/ThreadTypes.h"
#include "domain/net/ServiceClients.h"
#include "domain/net/BlobQuotaUtil.h"
#include "domain/people/ContactTypes.h"
#include "common/directory/IdentityTypes.h"
#include "domain/people/ProfileIdentityView.h"
#include "domain/ui/ChatWidgetTypes.h"
#include "domain/messaging/PaymentPromiseLifecycle.h"
#include "domain/messaging/PaymentPromiseWireCodec.h"
#include "foundation/data/PaymentPromiseTypes.h"
#include "common/Error.h"
#include "feature/conversations/AgentInboundPorts.h"
#include "feature/conversations/ChatSyncService.h"
#include "feature/conversations/MessageRouter.h"
#include "feature/conversations/MessagingUiPorts.h"
#include "feature/conversations/MeshMessagingService.h"
#include "domain/messaging/AnnounceLiveJoin.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "feature/conversations/PeerDisplayResolver.h"
#include "domain/messaging/PskSessionCoordinator.h"
#include "domain/mesh/reachability/Reachability.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class ConversationsHub;

/**
 * Non-owning wrapper around ConversationsHub&, held by Application and handed to
 * ChatController / chat sub-presenters and messaging tools. Replaces the
 * MessagingChatPorts mega-struct: imperative ops are real methods, event
 * subscriptions keep std::function params. Feature/chat depends only on this
 * facade, never on ConversationsHub directly.
 */
class ConversationsFacade {
public:
  explicit ConversationsFacade(ConversationsHub& hub);

  // --- Core / lifecycle -----------------------------------------------------
  MessagingView Snapshot();
  bool HasRouter();
  bool IsInitialized();
  bool IsMessagingReady();
  IThreadStore* ThreadStore();
  void BindAgentInbound(AgentInboundPorts ports);
  Roe<bool> MaybeAutoRenewRegistration(bool auto_renew_registration);
  Roe<void> SyncPushDevices(bool show_notifications);
  Roe<std::string> ExportLinkDevice();
  Roe<void> ImportLinkDevice(const std::string& bundle_json, const std::string& pin);
  void SuspendMeshColdPeers();

  // --- Inbox ----------------------------------------------------------------
  const std::string& ActiveThreadId();
  Roe<Thread> GetActiveThread();
  Roe<std::vector<Thread>> ListThreads();
  Roe<Thread> OpenThread(const std::string& thread_id);
  Roe<void> CloseThread(const std::string& thread_id);
  void ClearActiveThread();
  Roe<Thread> CreateNewAiThread();
  Roe<void> UpdatePreview(const std::string& thread_id, const std::string& preview);
  Roe<void> ClearThreadHistory(const std::string& thread_id, bool forget_memory);
  Roe<void> ForgetThreadMemory(const std::string& thread_id);
  Roe<void> SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title);
  Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel);
  void NotifyThreadChanged();
  void SetOnThreadChanged(std::function<void()> callback);
  PeerDisplayLabel ResolveThreadLabel(const Thread& thread);
  std::vector<MessageDisplayRow> BuildDisplayRows(const std::string& thread_id,
                                                  std::optional<int64_t> oldest_inclusive,
                                                  std::optional<int64_t> newest_inclusive = std::nullopt);
  bool HasLocalMessagesBefore(const std::string& thread_id, int64_t before_display_order);
  int SumUnread();

  // --- Store ----------------------------------------------------------------
  Roe<std::optional<Thread>> GetThread(const std::string& thread_id);
  Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string& thread_id, std::optional<int64_t> before,
                                                  int limit);
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message);
  Roe<bool> UpdateMessage(const ThreadMessage& message);
  Roe<uint32_t> GetChatTargetSessionEpoch(const std::string& thread_id);
  Roe<PeerSyncState> GetPeerSyncState(const std::string& thread_id, uint32_t epoch);

  // --- P2P ------------------------------------------------------------------
  void MaybeTailSync(const std::string& thread_id);
  Roe<ThreadMessage> SendUserMessage(const std::string& thread_id, const std::string& text, SendRelayOptions opts);
  Roe<ThreadMessage> SendReaction(const std::string& thread_id, const std::string& target_message_id,
                                  const std::string& emoji);
  Roe<ThreadMessage> ClearReaction(const std::string& thread_id, const std::string& target_message_id,
                                   const std::string& emoji);
  void SetOnMessagesChanged(std::function<void()> callback);
  void SetOnDeliveryNotice(std::function<void(const std::string&)> callback);
  void SetOnBackgroundUnread(std::function<void(std::string, std::string, std::string)> callback);
  void SyncInboxFromWake(bool force);
  void TailSyncActiveE2eThread();
  void WarmPeerForThread(const std::string& thread_id);
  ThreadPeerLinkView GetThreadPeerLink(const std::string& thread_id);
  void RetryPeerDial(const std::string& thread_id);
  void ScrollBackfill(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done);
  void SyncWithPeer(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done);
  void RetryGapSync(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_done);
  Roe<uint32_t> StartNewSecureChat(const std::string& thread_id);
  Roe<void> PauseIntegrityOnly(const std::string& thread_id);
  Roe<PskExportView> EnsurePskGenerated(const std::string& thread_id);
  Roe<PskExportView> GetPskExportView(const std::string& thread_id);
  Roe<PskSessionStatus> GetPskStatus(const std::string& thread_id);
  Roe<void> ImportPskBundleJson(const std::string& thread_id, const std::string& bundle_json);
  Roe<void> ImportPskRawBase64(const std::string& thread_id, const std::string& raw_b64);
  Roe<void> MarkPskVerified(const std::string& thread_id);
  Roe<std::string> RotatePskAndExportBundle(const std::string& thread_id);
  Roe<void> LockPublicThreadToThisDevice(const std::string& thread_id);
  Roe<PublicKeyScope> GetPublicKeyScope(const std::string& thread_id);
  Roe<bool> CanLockPublicToThisDevice(const std::string& thread_id);
  void SetSupportAccountId(std::string account_id);
  void RegisterContactDirectEndpoints(const Contact& contact);
  void RegisterPeerSigningKey(const std::string& kind, const std::string& value, const std::string& key_b64,
                              const std::string& source);
  void RegisterPeerKemKey(const std::string& kind, const std::string& value, const std::string& key_b64,
                          const std::string& source);
  Roe<Contact> AddContactFromDirectoryHit(const DirectoryHit& hit);
  Roe<RelayDeleteResult> ClearUndeliveredOlderThan(int older_than_days);

  // --- Groups ---------------------------------------------------------------
  Roe<void> DismissLocalGroup(const std::string& group_id);
  Roe<bool> IsLocalOwner(const std::string& group_id);
  Roe<std::vector<GroupRosterMember>> ListGroupRoster(const std::string& group_id);
  bool IsMemberUnreachable(const std::string& group_id, const std::string& member_identity);
  Roe<void> LeaveGroup(const std::string& group_id);
  Roe<void> LeaveAsOwner(const std::string& group_id, const std::string& new_owner_identity);
  Roe<std::string> OwnerIdentity(const std::string& group_id);
  bool IsOwnerUnreachable(const std::string& group_id);
  Roe<void> RenameGroupShared(const std::string& group_id, const std::string& title);
  Roe<void> RemoveMemberByIdentity(const std::string& group_id, const std::string& member_identity);
  std::vector<std::string> ListUnreachableMembers(const std::string& group_id);

  // --- Identity / contacts / directory --------------------------------------
  std::optional<LocalIdentity> GetIdentity();
  Roe<std::optional<Contact>> FindContactByIdentity(const std::string& identity, ContactIdKind kind);
  Roe<std::optional<Contact>> GetContact(const std::string& contact_id);
  std::optional<DirectoryHit> GetDirectoryShadow(const std::string& peer_id);

  // --- Actions --------------------------------------------------------------
  Roe<std::optional<std::string>> DispatchAction(const std::string& payload_json);
  void SetOnActionMessage(std::function<void(const std::string& message)> callback);

  // --- Router ---------------------------------------------------------------
  Roe<void> RouteMessage(const std::string& thread_id, const std::string& text,
                         std::optional<std::string> user_payload);
  bool ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                        const std::optional<std::string>& user_payload);

  // --- Pricing (P001) -------------------------------------------------------
  /** True when peer initiation floor > 0, relationship not open, and payment rails unavailable. */
  bool IsInitiationOutboundBlocked(const std::string& peer_identity);
  Roe<void> SendChargeRequired(const std::string& peer_identity,
                               std::optional<int64_t> floor_minor = std::nullopt);

  // --- Payment promises (P002 / P003) ----------------------------------------
  Roe<PaymentPromise> CreatePaymentPromiseOffer(const PaymentPromiseLifecycle::OfferParams& params);
  Roe<PaymentPromise> CreatePaymentPromiseOfferForThread(const std::string& thread_id,
                                                         PaymentPromiseLifecycle::OfferParams params);
  Roe<PaymentPromise> AcceptPaymentPromise(const std::string& promise_id);
  Roe<PaymentPromise> MarkPaymentPromiseDelivering(const std::string& promise_id);
  Roe<PaymentPromise> RecordPaymentPromiseOutcome(const std::string& promise_id, PaymentPromiseState outcome,
                                                  const std::string& note = {});
  Roe<void> AvoidPaymentPromiseCounterparty(const std::string& promise_id);
  Roe<std::vector<PaymentPromise>> ListPaymentPromises();
  Roe<std::optional<PaymentPromise>> GetPaymentPromise(const std::string& promise_id);
  Roe<std::vector<PaymentPromise>> ListPendingInboundPaymentPromises();
  Roe<std::optional<PaymentPromise>> GetPendingInboundPaymentPromise(const std::string& promise_id);
  Roe<PaymentPromise> AcceptInboundPaymentPromise(const std::string& promise_id);
  Roe<bool> IgnoreInboundPaymentPromise(const std::string& promise_id);
  bool ShouldAvoidPaymentCounterparty(const std::string& other_account_id);
  Roe<ThreadMessage> BuildPaymentPromiseControlMessage(const std::string& thread_id,
                                                       PaymentPromiseControlType type,
                                                       const PaymentPromise& promise,
                                                       const std::string& body_text);
  Roe<PaymentPromise> StagePaymentPromiseControlMessage(const ThreadMessage& message);
  void SetOnLocalAction(std::function<void(const std::string&, const std::optional<std::string>&)> callback);
  void SetSharedAiConfirmCallback(MessageRouter::SharedAiConfirmCallback callback);
  void MarkSharedAiConfirmed(const std::string& thread_id);

  // --- Settings / profile helpers (former Application hub peeks) -------------
  ProfileIdentityView LoadProfileIdentityView();
  Roe<void> SaveProfileNickname(const std::string& nickname);
  Roe<void> RegisterIdentity(const std::string& nickname);
  Roe<void> UploadProfileIconFromPath(const std::string& path);
  Roe<void> ClearProfileIcon();
  Roe<BlobQuotaRecoveryPlan> PlanRelayQuotaRecovery();
  Roe<void> FreeOldestRelayBlobSlot();
  void RequestAttachmentDownload(const std::string& thread_id, const std::string& message_id);
  void DrainPendingAttachmentMedia();
  Roe<void> ClearDownloadedAttachments();
  Roe<ThreadMessage> SendAttachmentFromPath(const std::string& thread_id, const std::string& path);
  void EnsureThreadAttachments(const std::string& thread_id);
  void RetryAttachmentDownload(const std::string& thread_id, const std::string& message_id);
  std::optional<std::string> AttachmentLocalPathForMessage(const std::string& thread_id,
                                                           const std::string& message_id);
  bool AttachmentOpenNeedsConfirmForMessage(const std::string& thread_id, const std::string& message_id);
  Roe<void> RotateBriefLlmKey();
  ReachabilitySnapshot Reachability();
  void RunReachabilityProbe(bool try_upnp);
  void TryUpnpPortMapping();
  std::string LastMeshError();
  /** Desktop Node "Help the network" posture (for mesh UX projections). */
  bool IsHelpNetworkEnabled();

  // --- Messaging tools helpers ----------------------------------------------
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query);
  Roe<std::vector<Contact>> SearchLocalContacts(const std::string& query);
  Roe<LocalIdentity> GetLocalIdentity();
  Roe<LocalIdentity> UpdateLocalIdentity(const LocalIdentity& identity);
  Roe<LocalIdentity> FinishAndPersistRegistration(const std::string& nickname);
  Roe<RegistrationResult> UpdateRegisteredNickname(const std::string& nickname);

  /**
   * Escape hatch for lifecycle / bridges that legitimately need the hub
   * (e.g. ConfigApplyBridge Apply slices). Prefer facade methods otherwise.
   */

  // --- Peer-scoped live announce (Spine C) ----------------------------------
  Roe<AnnounceLiveJoinPlan> PlanLiveJoinFromAnnounceTip(const PeerAnnounceTip& tip);
  Roe<AnnounceLiveJoinPlan> PlanLiveJoinFromStoredAnnounce(const std::string& peer_id,
                                                          const std::string& topic_id,
                                                          const std::string& program_id);
  /**
   * Plan from tip then arm pending invite via Calls(). No SoftMigrate/media.
   */
  Roe<PendingCallInvite> ArmLiveJoinFromAnnounceTip(const PeerAnnounceTip& tip);
  Roe<PendingCallInvite> ArmLiveJoinFromStoredAnnounce(const std::string& peer_id,
                                                       const std::string& topic_id,
                                                       const std::string& program_id);
  /** Accept an armed live-announce invite (no SoftMigrate / 1:1 media). */
  Roe<void> AcceptLiveAnnounceJoin(const std::string& call_id);
  /** Plan+arm from tip then accept (defers media when hop_peer_id absent). */
  Roe<PendingCallInvite> JoinLiveAnnounceFromTip(const PeerAnnounceTip& tip);

  ConversationsHub& Hub() { return hub_; }

private:
  ConversationsHub& hub_;
};

} // namespace pbr
