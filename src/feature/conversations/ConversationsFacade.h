#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "lib/messaging/AtAiParser.h"
#include "lib/messaging/GroupTypes.h"
#include "common/thread/IThreadStore.h"
#include "lib/messaging/SendRelayOptions.h"
#include "lib/messaging/ThreadLocalPrefsStore.h"
#include "common/thread/SyncStateTypes.h"
#include "common/thread/ThreadTypes.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/net/BlobQuota.h"
#include "lib/people/ContactTypes.h"
#include "common/directory/IdentityTypes.h"
#include "lib/people/ProfileIdentityView.h"
#include "lib/ui/ChatWidgetTypes.h"
#include "lib/pricing/PaymentPromiseLifecycle.h"
#include "lib/pricing/PaymentPromiseWireCodec.h"
#include "foundation/data/PaymentPromiseTypes.h"
#include "common/Error.h"
#include "feature/conversations/AgentInboundPorts.h"
#include "feature/conversations/ChatSyncWorkflow.h"
#include "feature/conversations/MessageRouter.h"
#include "feature/conversations/MessagingUiPorts.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "domain/peers/PeerDisplaySources.h"
#include "lib/messaging/PskSessionCoordinator.h"
#include "mesh/reachability/Reachability.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

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
  bool IsMeshReady();
  IThreadStore* ThreadStore();
  void BindAgentInbound(AgentInboundPorts ports);
  Roe<bool> MaybeAutoRenewRegistration(bool auto_renew_registration);
  Roe<void> SyncPushDevices();
  Roe<std::string> ExportLinkDevice();
  Roe<void> ImportLinkDevice(const std::string& bundle_json, const std::string& pin);
  /** Local account backup (uninstall-surviving store). Nickname works before any vault exists. */
  Roe<std::optional<std::string>> LocalBackupNickname();
  Roe<void> RestoreLocalBackup(const std::string& pin);
  /** Rewrite the local account backup on a worker (e.g. after a PIN change). Best effort. */
  void WriteLocalBackupAsync();
  Roe<void> DeleteLocalBackup();
  void SuspendMeshColdPeers();

  // --- Inbox ----------------------------------------------------------------
  const std::string& ActiveThreadId();
  Roe<Thread> GetActiveThread();
  Roe<std::vector<Thread>> ListThreads();
  /** True when an active local call is for this thread (origin / peer / group). */
  bool ThreadHasActiveCall(const std::string& thread_id);
  /** Calls answer ThreadHasActiveCall (feature-level wiring sets it; none: never). */
  void SetActiveCallQuery(std::function<bool(const Thread& thread)> query);
  Roe<Thread> OpenThread(const std::string& thread_id);
  Roe<void> CloseThread(const std::string& thread_id);
  void ClearActiveThread();
  Roe<Thread> CreateNewAiThread();
  Roe<void> UpdatePreview(const std::string& thread_id, const std::string& preview);
  Roe<void> ClearThreadHistory(const std::string& thread_id, bool forget_memory);
  Roe<void> DeleteMessageForMe(const std::string& thread_id, const std::string& message_id);
  Roe<std::optional<ThreadMessage>> GetMessage(const std::string& thread_id, const std::string& message_id);
  /** Our own message, for everyone in the chat / group. Blocking (network): call off the UI thread. */
  Roe<void> DeleteMessageForEveryone(const std::string& thread_id, const std::string& message_id);
  void MarkThreadRead(const std::string& thread_id);
  /** Per-thread local settings (pin / mute / archive); rows that differ from the defaults. */
  Roe<std::vector<ThreadLocalPrefs>> ListThreadLocalPrefs();
  Roe<ThreadLocalPrefs> GetThreadLocalPrefs(const std::string& thread_id);
  Roe<void> SetThreadLocalPrefs(const ThreadLocalPrefs& prefs);
  Roe<void> ForgetThreadMemory(const std::string& thread_id);
  Roe<void> SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title);
  /** Names an AI thread after its first question; a no-op once it has any other title. No thread-changed notify. */
  Roe<void> NameAiThreadFromFirstMessage(const std::string& thread_id, const std::string& title);
  Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel);
  void NotifyThreadChanged();
  void SetOnThreadChanged(std::function<void()> callback);
  PeerDisplayLabel ResolveThreadLabel(const Thread& thread);
  /** The thread's transcript (D031 window); the GUI draws it with TranscriptFactsFor(). */
  std::vector<TranscriptRow> BuildTranscript(const std::string& thread_id, std::optional<int64_t> oldest_inclusive,
                                             std::optional<int64_t> newest_inclusive = std::nullopt);
  TranscriptFacts TranscriptFactsFor();
  /** Active profile data directory (stores, session plaintext views). */
  const std::string& ProfileDataDir() const;
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
  /** Share contact `contact_id` into a chat as a contact card (ACCOUNT_CARD.md): its account's card, its name. */
  Roe<ThreadMessage> ShareContact(const std::string& thread_id, const std::string& contact_id);
  /** *Add* on a contact card in a chat: checks the card, keeps the account's keys, adds (or finds) the contact. */
  Roe<Contact> AddContactFromCard(const std::string& thread_id, const std::string& message_id);
  Roe<ThreadMessage> SendReaction(const std::string& thread_id, const std::string& target_message_id,
                                  const std::string& emoji);
  Roe<ThreadMessage> ClearReaction(const std::string& thread_id, const std::string& target_message_id,
                                   const std::string& emoji);
  void SetOnMessagesChanged(std::function<void()> callback);
  void SetOnDeliveryNotice(std::function<void(const DeliveryNotice&)> callback);
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
  /** The keys a directory entry carries for its account, each kept only when it proves to be the account's. */
  void RegisterPeerKeysFromDirectory(const mesh::DirectoryHit& hit);
  Roe<Contact> AddContactFromDirectoryHit(const mesh::DirectoryHit& hit);
  /** Block / unblock / trust from wherever the person is shown (chat row menu, contact detail). */
  Roe<void> SetContactTrust(const std::string& contact_id, TrustLevel level);
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
  Roe<std::optional<Contact>> FindContactByIdentity(const std::string& identity, mesh::ContactIdKind kind);
  Roe<std::optional<Contact>> GetContact(const std::string& contact_id);
  std::optional<mesh::DirectoryHit> GetDirectoryShadow(const std::string& peer_id);
  /** Start a directory lookup for `peer_id` if none is cached or in flight; the result lands in the shadow cache. */
  void EnsureDirectoryLookup(const std::string& peer_id);

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
  void SendAttachmentFromPathAsync(const std::string& thread_id, const std::string& path,
                                   std::function<void(Roe<ThreadMessage>)> on_done);
  void EnsureThreadAttachments(const std::string& thread_id);
  void RetryAttachmentDownload(const std::string& thread_id, const std::string& message_id);
  std::optional<std::string> AttachmentLocalPathForMessage(const std::string& thread_id,
                                                           const std::string& message_id);
  bool AttachmentOpenNeedsConfirmForMessage(const std::string& thread_id, const std::string& message_id);
  struct StoredAiImage {
    std::vector<uint8_t> content_hash;
    std::string view_path;
  };
  /** An image the user sends to the AI: private CAS + this thread's view file. Fails while locked. */
  Roe<StoredAiImage> StoreAiImage(const std::string& thread_id, const std::vector<uint8_t>& bytes,
                                  const std::string& mime);
  /** View file of a stored AI image, materialized from the CAS when missing; nullopt while locked or gone. */
  std::optional<std::string> AiImageViewPath(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                             const std::string& mime);
  Roe<void> RotateBriefLlmKey();
  mesh::ReachabilitySnapshot Reachability();
  void RunReachabilityProbe(bool try_upnp);
  void TryUpnpPortMapping();
  std::string LastMeshError();
  /** Desktop Node "Help the network" posture (for mesh UX projections). */
  bool IsHelpNetworkEnabled();

  // --- Messaging tools helpers ----------------------------------------------
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& query);
  Roe<std::vector<Contact>> SearchLocalContacts(const std::string& query);
  Roe<LocalIdentity> GetLocalIdentity();
  Roe<LocalIdentity> UpdateLocalIdentity(const LocalIdentity& identity);
  Roe<LocalIdentity> FinishAndPersistRegistration(const std::string& nickname);
  Roe<RegistrationResult> UpdateRegisteredNickname(const std::string& nickname);
  /** #328: take this account off the directory (profile reset / delete account). */
  Roe<void> RevokeRegistration();

  /**
   * Escape hatch for lifecycle / bridges that legitimately need the hub
   * (e.g. ConfigApplyBridge Apply slices). Prefer facade methods otherwise.
   */

  ConversationsHub& Hub() { return hub_; }

private:
  ConversationsHub& hub_;
  std::function<bool(const Thread& thread)> active_call_query_;
};

} // namespace pbr
