#pragma once

#include "common/Secret.h"

#include "foundation/crypto/CryptoTypes.h"
#include "lib/delivery/TextSpans.h"
#include "lib/messaging/AtAiParser.h"
#include "domain/attachments/OutgoingAttachmentStore.h"
#include "domain/attachments/StoredAttachments.h"
#include "lib/messaging/AttachmentLimits.h"
#include "lib/messaging/OutgoingAttachment.h"
#include "lib/messaging/GroupTypes.h"
#include "feature/conversations/GroupInviteLinkWorkflow.h"
#include "common/thread/IThreadStore.h"
#include "lib/delivery/SendRelayOptions.h"
#include "lib/messaging/IThreadLocalPrefsStore.h"
#include "lib/messaging/IThreadMentionsStore.h"
#include "common/thread/SyncStateTypes.h"
#include "common/thread/ThreadTypes.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/net/BlobQuota.h"
#include "lib/net/ProfileIconClient.h"
#include "lib/people/ContactTypes.h"
#include "common/directory/IdentityTypes.h"
#include "lib/people/ProfileIdentityView.h"
#include "lib/pricing/PaymentPromiseLifecycle.h"
#include "lib/pricing/PaymentPromiseWireCodec.h"
#include "lib/pricing/PaymentPromiseTypes.h"
#include "common/Error.h"
#include "feature/conversations/ActionNotice.h"
#include "feature/conversations/ContactCardExchange.h"
#include "feature/conversations/GroupInviteBatch.h"
#include "feature/conversations/AgentInboundPorts.h"
#include "feature/conversations/ChatSyncWorkflow.h"
#include "feature/conversations/MessageRouter.h"
#include "feature/conversations/MessagingUiPorts.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "lib/delivery/PskSessionCoordinator.h"
#include "mesh/reachability/Reachability.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <map>
#include <set>
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
  /**
   * MaybeAutoRenewRegistration in two halves, so the network half can run off the UI thread: nullopt when nothing is
   * due, else the finished registration; nothing stored. PersistRegistrationRenewal then reads the identity again and
   * saves only the registration fields.
   */
  Roe<std::optional<RegistrationResult>> MaybeAutoRenewRegistrationNetworkOnly(bool auto_renew_registration);
  /** `expected_account_id`: the account the network half ran for; another one stored now is refused. */
  Roe<LocalIdentity> PersistRegistrationRenewal(const RegistrationResult& result,
                                                const std::string& expected_account_id);
  /** Blocking (HTTP): binds this device's push tokens. Called off the UI thread (gui MessagingReadyTasks). */
  Roe<void> SyncPushDevices();
  /** Whether the saved backend differs from the running one: push is unbound from the running backend meanwhile. */
  void SetBackendSwitchPending(bool pending);
  Roe<std::string> ExportLinkDevice();
  /**
   * The account's recovery phrase (multi-device M023 §2), once `pin` proves to be this profile's PIN; nullopt when the
   * account has no recovery seed (minted before M023). A wrong PIN is a Pin-category error.
   */
  Roe<std::optional<SecretText>> RecoveryPhrase(const SecretText& pin);
  /** Whether the account has a recovery phrase to show (needs the vault unlocked; false otherwise). */
  bool HasRecoveryPhrase();
  Roe<void> ImportLinkDevice(const std::string& bundle_json, const SecretText& pin);
  /** Fresh install: the account back from its recovery phrase (M023; ConversationsHub::RestoreFromRecoveryPhrase). */
  Roe<void> RestoreFromRecoveryPhrase(const SecretText& phrase, const SecretText& pin,
                                      const std::vector<std::string>& saved_copies = {});
  /** The account's state sealed as an exported locker file's text (M023 §6 amendment; the user keeps it). */
  Roe<std::string> ExportAccountCopy();
  /** The account's devices (M021 d4): the newest list, its first the main device; whether this one is signed out. */
  Roe<OwnDeviceList> OwnDevices();
  /** On the main device: take another device off the account. */
  Roe<void> RemoveOwnDevice(const std::string& peer_id);
  /** On the main device: make another device the main one. */
  Roe<void> MakeMainDevice(const std::string& peer_id);
  void SuspendMeshColdPeers();

  // --- Inbox ----------------------------------------------------------------
  const std::string& ActiveThreadId();
  Roe<Thread> GetActiveThread();
  /** One more unread in `thread_id` (an AI answer that arrived while its page was not shown). */
  void IncrementUnread(const std::string& thread_id);
  Roe<std::vector<Thread>> ListThreads();
  void KeepThreadListed(const std::string& thread_id);
  /** True when an active local call is for this thread (origin / peer / group). */
  bool ThreadHasActiveCall(const std::string& thread_id);
  /** Calls answer ThreadHasActiveCall (feature-level wiring sets it; none: never). */
  void SetActiveCallQuery(std::function<bool(const Thread& thread)> query);
  /** A call is ringing here or this device is in one (any thread). False when no calls are wired. */
  bool AnyCallLive();
  void SetAnyCallQuery(std::function<bool()> query);
  Roe<Thread> OpenThread(const std::string& thread_id);
  Roe<void> CloseThread(const std::string& thread_id);
  void ClearActiveThread();
  Roe<Thread> CreateNewAiThread();
  /** A new AI thread, stored but not opened (the ask-AI panel's conversation when there is none yet). */
  Roe<Thread> CreateAiThread();
  Roe<void> UpdatePreview(const std::string& thread_id, const std::string& preview, bool assistant_text = false);
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
  Roe<void> NameAiThreadFromFirstMessage(const std::string& thread_id, const std::string& first_message);
  Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel);
  void NotifyThreadChanged();
  void SetOnThreadChanged(std::function<void()> callback);
  PeerDisplayLabel ResolveThreadLabel(const Thread& thread);
  /** The name a message's sender (a contact id, or a group member's account id) shows; `trust` says how sure it is. */
  PeerDisplayLabel ResolveSenderLabel(const std::string& sender_contact_id);
  /**
   * Local file of a saved contact's cached profile photo, empty when none is cached yet (the letter avatar shows).
   * Starts the photo fetch when the contact has one that is not cached, like the Contacts list does.
   */
  std::string ContactIconPath(const std::string& contact_id);
  /** The thread's transcript (D031 window); the GUI draws it with TranscriptFactsFor(). */
  std::vector<TranscriptRow> BuildTranscript(const std::string& thread_id, std::optional<int64_t> oldest_inclusive,
                                             std::optional<int64_t> newest_inclusive = std::nullopt);
  TranscriptFacts TranscriptFactsFor();
  /** Active profile data directory (stores, session plaintext views). */
  const std::string& ProfileDataDir() const;
  bool HasLocalMessagesBefore(const std::string& thread_id, int64_t before_display_order);
  int SumUnread();
  /** SumUnread(), or empty when the chats could not be listed (the app icon is then left alone). */
  std::optional<int> SumUnreadIfKnown();

  // --- Store ----------------------------------------------------------------
  Roe<std::optional<Thread>> GetThread(const std::string& thread_id);
  Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string& thread_id, std::optional<int64_t> before,
                                                  int limit);
  /** Every message of the thread, oldest first (search inside a chat). Reads and decrypts it all: off the UI thread. */
  Roe<std::vector<ThreadMessage>> GetMessages(const std::string& thread_id);
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message);
  Roe<bool> UpdateMessage(const ThreadMessage& message);
  Roe<uint32_t> GetChatTargetSessionEpoch(const std::string& thread_id);
  Roe<PeerSyncState> GetPeerSyncState(const std::string& thread_id, uint32_t epoch);

  // --- P2P ------------------------------------------------------------------
  void MaybeTailSync(const std::string& thread_id);
  Roe<ThreadMessage> SendUserMessage(const std::string& thread_id, const std::string& text, SendRelayOptions opts);
  Roe<ThreadMessage> SendGroupMessage(const std::string& thread_id, const std::string& text, SendRelayOptions opts);
  /**
   * A group text with marked ranges (mentions, `@everyone` — G015): spans checked (structure, ranges; a bad range
   * is dropped, the text stays) and sent in the ChatPayload (type 6); the row keeps them in `payload_json`.
   */
  Roe<ThreadMessage> SendGroupText(const std::string& thread_id, const std::string& text, std::vector<TextSpan> spans);
  /** The chats with an unread message that mentions us (G015 marker); empty when unknown. */
  std::set<std::string> ThreadsWithMentions();
  /** Those chats with how many unread mentions each has; empty when unknown. */
  std::map<std::string, int> MentionCounts();
  /** The oldest unread mention in a chat — where opening it can jump to; none when it has none. */
  std::optional<std::string> OldestMention(const std::string& thread_id);
  /** The newest mention over every chat, read or not (Home's Mentions widget); none when none or unknown. */
  std::optional<IThreadMentionsStore::Mention> LatestMention();
  /** Share contact `contact_id` into a chat as a contact card (ACCOUNT_CARD.md): its account's card, its name. */
  Roe<ThreadMessage> ShareContact(const std::string& thread_id, const std::string& contact_id);
  /** This account's locator, for a QR code or an invite line. */
  Roe<std::string> OwnContactLocator();
  /**
   * Adds the person a locator in `text` names (an invite line, a scanned code): the contact, then the
   * account's card from a listed device or the directory. `done` once, on a worker or I/O thread.
   */
  void AddContactFromLocator(const std::string& text, std::function<void(Roe<ContactCardExchange::LocatorAdded>)> done);
  /** *Add* on a contact card in a chat: checks the card, keeps the account's keys, adds (or finds) the contact. */
  Roe<Contact> AddContactFromCard(const std::string& thread_id, const std::string& message_id);
  Roe<ThreadMessage> SendReaction(const std::string& thread_id, const std::string& target_message_id,
                                  const std::string& emoji);
  Roe<ThreadMessage> ClearReaction(const std::string& thread_id, const std::string& target_message_id,
                                   const std::string& emoji);
  void SetOnMessagesChanged(std::function<void()> callback);
  void SetOnDeliveryNotice(std::function<void(const DeliveryNotice&)> callback);
  /** title, body, thread id, and whether the message mentions us (G015: alerts even in a muted group). */
  void SetOnBackgroundUnread(std::function<void(std::string, std::string, std::string, bool)> callback);
  /** (message id, thread id) of each inbound message stored, on the UI thread. */
  void SetOnInboundMessage(std::function<void(std::string, std::string)> callback);
  /** The chat a recent inbound message (or, by call id, a call summary) was stored in. */
  std::optional<std::string> ThreadOfInbound(const std::string& id) const;
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
  /** G016: the owner ends the group for everyone; its chat stays here, read-only. */
  Roe<void> DissolveGroup(const std::string& group_id);
  bool IsGroupDissolved(const std::string& group_id);
  Roe<bool> IsLocalOwner(const std::string& group_id);
  Roe<std::vector<GroupRosterMember>> ListGroupRoster(const std::string& group_id);
  bool IsMemberUnreachable(const std::string& group_id, const std::string& member_identity);
  Roe<void> LeaveGroup(const std::string& group_id);
  Roe<void> LeaveAsOwner(const std::string& group_id, const std::string& new_owner_identity);
  Roe<std::string> OwnerIdentity(const std::string& group_id);
  /** The nickname on the account card kept for `account_id` (a member's own, public); empty if none. */
  std::string KeptCardNickname(const std::string& account_id);
  bool IsOwnerUnreachable(const std::string& group_id);
  Roe<void> RenameGroupShared(const std::string& group_id, const std::string& title);
  Roe<void> RemoveMemberByIdentity(const std::string& group_id, const std::string& member_identity);
  /** G017, owner-only: make a member an admin, or a member again (GroupMembershipWorkflow::SetMemberRole). */
  Roe<void> SetMemberRole(const std::string& group_id, const std::string& member_identity, MemberRole role);
  /** G017, owner-only: set the group Open or Managed. */
  Roe<void> SetGroupPreset(const std::string& group_id, GroupPreset preset);
  Roe<GroupPreset> GroupPresetOf(const std::string& group_id);
  /** Our role in the group (Owner by its recorded owner); none when we are not in it. */
  std::optional<MemberRole> LocalGroupRole(const std::string& group_id);
  /** G019 5: the members `account` brought in, as the roster holds it now. */
  std::vector<std::string> GroupMembersAddedBy(const std::string& group_id, const std::string& account);
  /** G020 6: the accounts `account` removed who are still out. */
  std::vector<std::string> GroupAccountsRemovedBy(const std::string& group_id, const std::string& account);
  /** G020 6, owner and admins: remove everyone `account` added who is ours to remove; how many removals went out. */
  Roe<size_t> RemoveEveryoneAddedBy(const std::string& group_id, const std::string& account);
  /** G020 6, the owner: invite again everyone `account` removed; how many invites went out. */
  Roe<size_t> ReinviteEveryoneRemovedBy(const std::string& group_id, const std::string& account);
  /** G019: invite by account (someone not a contact, e.g. met in the group), per the preset. */
  Roe<void> InviteIdentity(const std::string& group_id, const std::string& account);
  // G021, invite links (GroupInviteLinkWorkflow).
  /** A new link to the group (per the preset); `lifetime_ms` 0: 7 days; `max_uses` 0: no limit. Blocking. */
  Roe<GroupInviteLinkWorkflow::MadeLink> MakeInviteLink(const std::string& group_id, uint64_t max_uses = 0,
                                                        int64_t lifetime_ms = 0);
  Roe<void> RevokeInviteLink(const std::string& group_id, const std::string& link_id);
  /** The group's live links (ours with their text). */
  std::vector<GroupLinkRow> GroupInviteLinks(const std::string& group_id);
  /** Our newest live link to the group whose text we keep. */
  std::optional<GroupLinkRow> OwnInviteLink(const std::string& group_id);
  /** Ask to join by a pasted, scanned or opened `pp://join/…`: the request id (JoinByLinkState follows it). */
  Roe<std::string> AskToJoinByLink(const std::string& link_text);
  /** The preview's Join: the group's chat. */
  Roe<Thread> ConfirmJoinByLink(const std::string& request_id);
  void CancelJoinByLink(const std::string& request_id);
  std::optional<GroupJoinState> JoinByLinkState(const std::string& request_id);
  std::vector<std::string> ListUnreachableMembers(const std::string& group_id);
  /**
   * Whoever the preset lets invite (G019): send `contact_id` a normal group invite. Refuses when the roster already holds kGroupMemberLimit
   * members. Resolves the invitee, seals and writes on the calling thread: the UI thread uses InviteMembersAsync.
   */
  Roe<void> InviteMember(const std::string& group_id, const std::string& contact_id);
  /** UI thread: one InviteMember per contact on a worker (a failure does not stop the rest); `done` runs on the UI thread. */
  void InviteMembersAsync(const std::string& group_id, std::vector<std::string> contact_ids,
                          std::function<void(GroupInviteBatchResult)> done);

  // --- Identity / contacts / directory --------------------------------------
  std::optional<LocalIdentity> GetIdentity();
  /** A group member's public name — what a mention inserts and a group line shows (GroupMentions.h). */
  std::string MemberPublicNameOf(const std::string& account_id);
  Roe<std::optional<Contact>> FindContactByIdentity(const std::string& identity, mesh::ContactIdKind kind);
  Roe<std::optional<Contact>> GetContact(const std::string& contact_id);
  std::optional<mesh::DirectoryHit> GetDirectoryShadow(const std::string& peer_id);
  /** Start a directory lookup for `peer_id` if none is cached or in flight; the result lands in the shadow cache. */
  void EnsureDirectoryLookup(const std::string& peer_id);

  // --- Actions --------------------------------------------------------------
  Roe<std::optional<std::string>> DispatchAction(const std::string& payload_json);
  /** UI thread: Add / Message on a directory row look the person up first (HTTP), so `done` may run later. */
  void DispatchActionAsync(const std::string& payload_json, std::function<void(Roe<std::optional<std::string>>)> done);
  void SetOnActionMessage(std::function<void(const ActionNotice& notice)> callback);

  // --- Router ---------------------------------------------------------------
  Roe<void> RouteMessage(const std::string& thread_id, const std::string& text,
                         std::optional<std::string> user_payload);
  bool ExpectsAgentWork(const std::string& thread_id, const std::string& text,
                        const std::optional<std::string>& user_payload);

  // --- Pricing (P001) -------------------------------------------------------
  /** True when peer initiation floor > 0, relationship not open, and payment rails unavailable. */
  bool IsInitiationOutboundBlocked(const std::string& peer_identity);
  Roe<void> SendChargeRequired(const std::string& peer_identity, std::optional<int64_t> floor_minor = std::nullopt);

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
  Roe<ThreadMessage> BuildPaymentPromiseControlMessage(const std::string& thread_id, PaymentPromiseControlType type,
                                                       const PaymentPromise& promise, const std::string& body_text);
  void SetOnLocalAction(std::function<void(const std::string&, const std::optional<std::string>&)> callback);
  void SetSharedAiConfirmCallback(MessageRouter::SharedAiConfirmCallback callback);
  /** "Don't ask again" before sharing with the AI: kept for the chat across restarts. */
  void MarkSharedAiConfirmed(const std::string& thread_id);
  /** Undoes it: the next "@ai+" / "@ai++" in this chat asks again. */
  Roe<void> ClearSharedAiConfirmed(const std::string& thread_id);
  Roe<void> RouteConfirmedSharedAi(const std::string& thread_id, const std::string& text);
  bool NeedsSharedAiConfirmFor(const std::string& thread_id, const std::string& text) const;

  // --- Settings / profile helpers (former Application hub peeks) -------------
  ProfileIdentityView LoadProfileIdentityView();
  Roe<void> SaveProfileNickname(const std::string& nickname);
  Roe<void> RegisterIdentity(const std::string& nickname);
  Roe<void> UploadProfileIcon(const PreparedProfileIcon& icon);
  Roe<void> ClearProfileIcon();
  Roe<BlobQuotaRecoveryPlan> PlanRelayQuotaRecovery(const Error& quota_error);
  Roe<void> DeleteRelayFileById(const std::string& blob_id);
  void RequestAttachmentDownload(const std::string& thread_id, const std::string& message_id);
  /**
   * Me → Storage → Manage storage. Each runs on a worker and calls `done` on the UI thread: the attachments this
   * device holds, one per (chat, hash); the bytes deleting `items` would free; the delete itself (see
   * AttachmentFetchWorkflow::DeleteLocalAttachments); the bytes the message databases take.
   */
  void ListStoredAttachmentsAsync(std::function<void(std::vector<StoredAttachmentItem>)> done);
  void ReleasableAttachmentBytesAsync(std::vector<StoredAttachmentRef> items, std::function<void(uint64_t)> done);
  void DeleteLocalAttachmentsAsync(std::vector<StoredAttachmentRef> items,
                                   std::function<void(StoredAttachmentDeleteResult)> done);
  void MessageStoreBytesAsync(std::function<void(uint64_t)> done);
  /** A storage-grid cell's thumbnail, made on a background worker (returns at once; SetOnGridThumbnailReady). */
  void RequestGridThumbnail(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                            const std::string& mime, uint64_t page_generation);
  /** The page `page_generation` is left: its thumbnails not started yet are dropped. */
  void CancelGridThumbnails(uint64_t page_generation);
  /** The cell's thumbnail or video poster, when one exists (empty: none). A stat only. */
  std::string GridThumbnailPath(const std::string& thread_id, const std::vector<uint8_t>& content_hash);
  /** Called on the UI thread when an attachment arrives or goes. */
  void SetOnStoredAttachmentsChanged(std::function<void()> callback);
  /** Called on the UI thread when a storage-grid cell's thumbnail (RequestGridThumbnail) is made. */
  void SetOnGridThumbnailReady(
      std::function<void(const std::string& thread_id, const std::vector<uint8_t>& content_hash)> callback);
  /** Send bytes the picker already prepared (a shrunk picture, an original with its location removed). */
  void SendAttachmentAsync(const std::string& thread_id, OutgoingAttachment attachment,
                           std::function<void(Roe<ThreadMessage>)> on_done,
                           std::function<bool(uint64_t done, uint64_t total)> progress = {},
                           std::shared_ptr<const std::atomic<bool>> cancelled = {});
  void ResumeAttachmentAsync(const std::string& thread_id, const std::string& record_id,
                             std::function<void(Roe<ThreadMessage>)> on_done,
                             std::function<bool(uint64_t done, uint64_t total)> progress = {},
                             std::shared_ptr<const std::atomic<bool>> cancelled = {});
  std::vector<OutgoingAttachmentRecord> PendingOutgoingAttachments(const std::string& thread_id);
  void DiscardOutgoingAttachment(const std::string& record_id);
  void CleanStaleAttachmentFiles();
  /** The one source of every attachment size / pace / warning number (today the defaults). */
  AttachmentLimits CurrentAttachmentLimits() const;
  /** The server space is filling up: called on the UI thread with the percent used (said once per 80 / 90). */
  void SetOnBlobSpaceWarning(std::function<void(int percent)> callback);
  void EnsureThreadAttachments(const std::string& thread_id);
  void RetryAttachmentDownload(const std::string& thread_id, const std::string& message_id);
  struct AttachmentInfo {
    std::string mime;
    uint64_t byte_length = 0;
  };
  /** Declared type and size of an attachment message (none: not an attachment). */
  std::optional<AttachmentInfo> AttachmentInfoForMessage(const std::string& thread_id, const std::string& message_id);
  /** `made_view` (optional) is set when this call had to create the view file (it did not exist yet). */
  std::optional<std::string> AttachmentLocalPathForMessage(const std::string& thread_id, const std::string& message_id,
                                                           bool* made_view = nullptr);
  /** The same file under the sender's name (AttachmentNamedOpenPath): what the system viewer is handed. */
  std::optional<std::string> AttachmentOpenPathForMessage(const std::string& thread_id, const std::string& message_id,
                                                          bool* made_view = nullptr);
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
  Roe<void> RotateBriefLlmKey(const std::string& llm_base_url);
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
  /** One read-modify-write under the identity store's lock (IdentityStore::Modify); for writers off the UI thread. */
  Roe<LocalIdentity> ModifyLocalIdentity(const std::function<bool(LocalIdentity& identity)>& change);
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
  std::function<bool()> any_call_query_;
};

} // namespace pbr
