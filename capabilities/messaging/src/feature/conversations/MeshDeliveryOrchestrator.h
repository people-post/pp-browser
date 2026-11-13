#pragma once

#include "lib/delivery/RelayClients.h"
#include "mesh/host/DisclosureGatedPeerLinks.h"
#include "common/Module.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "common/thread/IThreadStore.h"
#include "lib/pricing/InitiationBillingStore.h"
#include "lib/pricing/PaymentPromiseStore.h"
#include "lib/messaging/CardAsk.h"
#include "lib/messaging/CardAskGate.h"
#include "lib/messaging/CopyAsk.h"
#include "lib/messaging/IThreadMentionsStore.h"
#include "lib/messaging/SentCopy.h"
#include "lib/delivery/DeliveryNotice.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/people/PeerKemKeyStore.h"
#include "lib/delivery/IPeerSessionStore.h"
#include "lib/people/PeerSigningKeyStore.h"
#include "lib/delivery/SendRelayOptions.h"
#include "feature/conversations/ChatSyncWorkflow.h"
#include "domain/peers/AccountKeyResolver.h"
#include "domain/peers/OwnCards.h"
#include "lib/delivery/OutboundMessage.h"
#include "domain/delivery/AccountCardService.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "lib/messaging/EpochBumpCoordinator.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/InboundThreadIndex.h"
#include "common/chat/IDirectMessageClient.h"
#include "lib/delivery/PskSessionCoordinator.h"
#include "lib/delivery/PublicPskLockCoordinator.h"
#include "lib/delivery/E2ePublicSessionLogic.h"
#include "common/chat/SystemControl.h"
#include "lib/delivery/IControlLaneSeenStore.h"
#include "domain/delivery/RelayGrantCoordinator.h"
#include "domain/delivery/ControlLane.h"
#include "domain/delivery/DeliveryOutbox.h"
#include "domain/delivery/E2ePublicSessionKeys.h"
#include "feature/conversations/PeerTransports.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/GroupInviteGate.h"
#include "mesh/host/MeshPorts.h"
#include "lib/net/OrgBackendClients.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class GroupMembershipWorkflow;
class AttachmentFetchWorkflow;
class IChatHistoryPeerClient;
class IChatBlobPeerClient;

/** Product-facing path for chat header chrome (not dial/punch mechanics). */
enum class ThreadPeerPathKind {
  None = 0,
  Direct,     // live ADP PeerLink
  ViaHop,     // live circuit/media carrier PeerLink
  ViaRelay,   // Brief/HTTP message relay only (no live mesh link)
  Connecting, // dial / handshake / punch — short-lived
  Degraded,   // backoff / retry soon
  Failed,     // offline / can't connect
  Ready,      // idle, not yet dialing
};

/** Aggregated peer-link UX for a direct chat thread. */
struct ThreadPeerLinkView {
  mesh::MeshPeerLinkPhase phase = mesh::MeshPeerLinkPhase::Unavailable;
  ThreadPeerPathKind path_kind = ThreadPeerPathKind::None;
  /** The transport's own failure text, for the log (the GUI words the state — PeerLinkText). */
  std::string detail;
  /** A retry may help (backing off after a failed dial). */
  bool show_retry = false;
  int backoff_seconds = 0;
  bool has_direct_endpoint = false;
  /** Messages can go via the Brief relay (the peer has an Account ID, its inbox, and a relay is configured). */
  bool relay_available = false;
  bool carrier_backed = false;
};

class MeshDeliveryOrchestrator : public Module {
public:
  /** Amp single entry for chat / history / blob ([A020]); requires `amp_links`. */
  MeshDeliveryOrchestrator(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity, IRelayClient* relay,
                           InboxController& inbox, AccountKeyResolver& account_keys, IPeerSessionStore& psk_store,
                           IGroupRosterStore& group_roster, GroupInviteGate* invite_gate = nullptr,
                           mesh::IChatPeerLinks* amp_links = nullptr, std::function<void()> amp_io_pump = {},
                           std::function<void(std::function<void()>)> amp_worker_post = {},
                           std::function<void(std::function<void()>)> amp_post_io = {},
                           std::function<void(std::chrono::milliseconds, std::function<void()>)> amp_post_after = {});

  Roe<ThreadMessage> SendUserMessage(const std::string& thread_id, const std::string& text,
                                     const SendRelayOptions& options = {});
  Roe<ThreadMessage> SendGroupMessage(const std::string& thread_id, const std::string& text,
                                      const SendRelayOptions& options = {});
  /**
   * P001: re-lock peer initiation billing (`charge_required` system message) then MarkClosed locally.
   * `floor_minor` defaults to local identity initiation_floor when nullopt.
   */
  Roe<void> SendChargeRequired(const std::string& peer_identity, std::optional<int64_t> floor_minor = std::nullopt);
  InitiationBillingStore* InitiationBilling() const { return initiation_billing_; }
  /** D098 — append reaction / reaction_clear annotation on direct or group thread. */
  Roe<ThreadMessage> SendReaction(const std::string& thread_id, const std::string& target_message_id,
                                  const std::string& emoji);
  /** "Delete for everyone": tell the peer / group to delete our message `target_message_id`. */
  Roe<ThreadMessage> SendRetract(const std::string& thread_id, const std::string& target_message_id);
  /**
   * Our edit of our message `target_message_id`: an `edit` annotation with the whole new `text`, as `message_id`, sent
   * at `sent_at_ms` (fixed by the caller; DEV/PLAN-message-edit.md). The row stays when the send fails after it was
   * stored (it is marked failed); an error before that stores nothing.
   */
  Roe<ThreadMessage> SendEdit(const std::string& thread_id, const std::string& target_message_id,
                              const std::string& text, const std::string& message_id, int64_t sent_at_ms);
  /** A deleted message that has not gone out yet is not retried. */
  void CancelPendingSend(const std::string& message_id);
  /** When the send of our message `message_id` still under way started (DeliveryOutbox::SendStartedMs). Any thread. */
  std::optional<int64_t> SendStartedMs(const std::string& message_id) const {
    return outbox_.SendStartedMs(message_id);
  }
  Roe<ThreadMessage> ClearReaction(const std::string& thread_id, const std::string& target_message_id,
                                   const std::string& emoji);
  void PollAndMerge();
  /** Same ingest as PollAndMerge; `force` bypasses foreground rate limit. */
  void SyncInboxFromWake(bool force = true);
  /** Last HTTP Brief/relay poll outcome for ambient status chrome. */
  enum class BriefRelayHealthState {
    Unknown = 0,
    Ok = 1,
    Failed = 2,
  };
  BriefRelayHealthState BriefRelayHealth() const {
    return static_cast<BriefRelayHealthState>(brief_relay_health_.load(std::memory_order_relaxed));
  }
  /**
   * The relay health changed (Unknown/Ok/Failed): fired only on a change, on the poll worker thread, so keep the
   * callback short and thread-safe. Set before messaging runs; pass nullptr to clear (any thread).
   */
  /**
   * Forget the last poll result (back to Unknown) WITHOUT firing the callback: the owner just reset its own copy,
   * and the next result, Ok included, must count as a change or the owner never hears it.
   */
  void ResetBriefRelayHealth() {
    brief_relay_health_.store(static_cast<int>(BriefRelayHealthState::Unknown), std::memory_order_relaxed);
  }
  void SetOnBriefRelayHealthChanged(std::function<void(BriefRelayHealthState)> callback);
  /** Me recovery: delete undelivered relay rows older than `older_than_days` (does not clear local chat). */
  Roe<RelayDeleteResult> ClearUndeliveredOlderThan(int older_than_days);
  void RetryFailedOutbound();
  void SetRelayClient(IRelayClient* relay);
  /** The system-control families features bind (call control, …); passed on to the receive path. Not owned. */
  void SetSystemControls(const SystemControlRegistry* controls);
  void SetGroupMembership(GroupMembershipWorkflow* groups);
  void SetInitiationBillingStore(InitiationBillingStore* store);
  void SetProfileDataDir(std::string profile_data_dir);
  /** The profile's content store, for the blob transport (PeerTransports::SetContent). */
  void SetContent(std::shared_ptr<CasStore> content);
  /** The control lane's applied ids (the hub's store; null: none kept, every copy applies). */
  void SetControlLaneSeen(IControlLaneSeenStore* seen);
  void SetAttachmentDownloads(AttachmentFetchWorkflow* downloads);
  /** Where a live message that mentions us is marked (G015); null: not marked. */
  void SetThreadMentions(IThreadMentionsStore* mentions) { mentions_ = mentions; }
  /**
   * A group's member check settled (its roster changed): its held mentions are decided — those still under
   * kHeldMentionMaxAgeMs that reach us are marked and alert like a fresh one; the rest are dropped.
   */
  void DecideHeldMentions(const std::string& group_id);
  /**
   * Bind Amp chat/history/blob on an existing orchestrator.
   * Prefer this over destroying/recreating the orchestrator after messaging is ready —
   * in-flight SyncInbox / PostWorker lambdas still hold `this`.
   */
  /**
   * projects/privacy T1: chat, attachments and history dial only peers this gate allows (others over
   * a link that is already up, else the Brief relay). Set before AttachAmpTransports. Not owned.
   */
  void SetAddressDisclosure(const mesh::AddressDisclosureGate* gate) {
    transports_.SetAddressDisclosure(gate);
    card_service_.SetAudienceEveryone(
        [gate]() { return gate && gate->Snapshot()->audience == mesh::DirectAudience::Everyone; });
  }
  /** The relay recipient for `account_id`: the Account ID itself (its inbox, MR001); none for another kind of id. */
  static std::optional<std::string> RelayRecipientFor(const std::string& account_id);
  /** This account's relay grants: issued to who we accept, held from who accepted us (MR005). */
  RelayGrantCoordinator& RelayGrants() { return relay_grants_; }
  /** Where the relay grants are kept (the profile's store); none: none are issued or attached. */
  void SetRelayGrantStore(IRelayGrantStore* store);
  /** Account cards between devices (the card exchange over Amp). */
  AccountCardService& AccountCards() { return card_service_; }
  /** This account's own key card and device list (M021). */
  OwnCards& OwnAccountCards() { return own_cards_; }
  /**
   * The newest card this device holds per account, for checking group changes (G018 item 9): the kept cards, and our
   * own account's.
   */
  GroupCardLookup GroupChangeCards();
  /** Our device list changed (our address goes to our other devices too, M021). */
  void SetOnOwnDevicesChanged(std::function<void()> changed) { on_own_devices_changed_ = std::move(changed); }
  /** A verified notice from another device of our own account arrived (M023: time to sync the locker). */
  /** G016: a group ended by an answer or another device of ours: its chat gets the one unread count, preview and notice. */
  void SurfaceGroupDissolved(const std::string& thread_id);
  void SetOnSiblingNotice(std::function<void()> callback) { on_sibling_notice_ = std::move(callback); }
  /**
   * Our other devices hear that something of the account changed here (the account locker was written, M023): the
   * same `device_list` notice through our own mailbox — receiving any sibling notice prompts a sync.
   */
  void NudgeSiblings();
  /**
   * A control notice to our own account's other devices (one record to our own mailbox, sealed to the account
   * KEM): applied there by its family (M021 d5: a call taken here). Nothing when our card lists only us.
   */
  Roe<void> NotifyOwnDevices(const ThreadMessage& control);
  /**
   * What we sent shows on our other devices (M021 d3): a sent-copy notice to our own mailbox, when our card lists
   * other devices. Off the caller's thread.
   */
  void MirrorToSiblings(const ThreadMessage& sent, SentCopyTarget target);
  /**
   * A copy may be missing for this device (M021 §9): once kMissingCopyWaitMs passed without the message arriving
   * another way (a copy over a link), ask the sending device for one (`copy_ask`, carrying our card). Once per
   * message.
   */
  void NoteMissingCopy(const RelayReceiveOutcome::MissingCopy& missing);
  /**
   * Another account's device asks for a copy of a message we sent: if this device sent it and the asking device
   * is on the asker's card, seal it one copy in their device pair and send it (direct or group).
   */
  void AnswerCopyAsk(const CopyAsk& ask, const std::string& asker_account);
  /** M021 d6: another account asks for our newer card (`card_ask`): the device named sends it (`device_list`). */
  void AnswerCardAsk(const std::string& to_device, const std::string& asker_account);
  void AttachAmpTransports(mesh::IChatPeerLinks* amp_links, std::function<void()> amp_io_pump = {},
                           std::function<void(std::function<void()>)> amp_worker_post = {},
                           std::function<void(std::function<void()>)> amp_post_io = {},
                           std::function<void(std::chrono::milliseconds, std::function<void()>)> amp_post_after = {});
  /**
   * Stop Amp protocol handlers and drop transport objects while MeshHost/Amp is still alive.
   * Required before MeshHost::Stop — otherwise ~Amp*Transport::Stop UAFs PeerLinks.
   */
  void DetachAmpTransports();
  /** R019 peer-direct attachment blobs (null when mesh unavailable). */
  IChatBlobPeerClient* PeerBlobClient() const;
  IChatBlobPeerService* PeerBlobService() const;
  void SetOnMessagesChanged(std::function<void()> callback);
  void NotifyMessagesChanged();
  void SetOnDeliveryNotice(std::function<void(const DeliveryNotice&)> callback);
  /**
   * Fired on UI thread when a background ingest bumps unread (for OS notify). `mentions_us`: a group message
   * that mentions us (G015, MessageMentionsUs) — it alerts even in a muted group, rate-limited.
   */
  void SetOnBackgroundUnread(
      std::function<void(std::string title, std::string body, std::string thread_id, bool mentions_us)> callback);
  /** Fired on UI thread per inbound message stored in a thread (message id, thread id), as it is synced. */
  void SetOnInboundMessage(std::function<void(std::string message_id, std::string thread_id)> callback);
  /** Where a recent inbound message (by message id) or call (by call id, once its summary arrived) went. */
  std::optional<std::string> ThreadOfInbound(const std::string& id) const { return inbound_threads_.Find(id); }
  /**
   * The keys a directory entry carries for its account: the signing key (kept when it hashes to the
   * Account ID) and the KEM key (kept when the account bound it to itself). Either store refuses the rest.
   */
  void RegisterPeerKeysFromDirectory(const mesh::DirectoryHit& hit);

  /** Spine B: local tip publisher (device ML-DSA); null when Amp/identity not ready. */

  void MaybeTailSync(const std::string& thread_id);
  /** D059 — full user-initiated sync (async on IO thread). */
  void SyncWithPeer(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_complete = {});
  /** D059 — gap banner retry (async on IO thread). */
  void RetryGapSync(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_complete = {});
  /** D038/D068 — epoch-only bump to recover from compromised state. */
  Roe<uint32_t> StartNewSecureChat(const std::string& thread_id);
  /** E020 — rotate PSK, bump epoch, return OOB bundle JSON. */
  Roe<std::string> RotatePskAndExportBundle(const std::string& thread_id);
  /** E027 — public 1:1 in-band device-lock (not private OOB rotate). */
  Roe<void> LockPublicThreadToThisDevice(const std::string& thread_id);
  Roe<PublicKeyScope> GetPublicKeyScope(const std::string& thread_id) const;
  Roe<bool> CanLockPublicToThisDevice(const std::string& thread_id) const;
  /** PP Support Account id from client-compat; empty disables Support lock gate. */
  void SetSupportAccountId(std::string account_id);
  const std::string& SupportAccountId() const { return support_account_id_; }
  /** D038 — pause ingest/outbound without rotating keys. */
  Roe<void> PauseIntegrityOnly(const std::string& thread_id);

  Roe<PskSessionStatus> GetPskStatus(const std::string& thread_id) const;
  Roe<PskExportView> EnsurePskGenerated(const std::string& thread_id);
  Roe<PskExportView> GetPskExportView(const std::string& thread_id) const;
  Roe<std::string> ExportPskBundleJson(const std::string& thread_id) const;
  Roe<void> ImportPskRawBase64(const std::string& thread_id, const std::string& raw_b64);
  Roe<void> ImportPskBundleJson(const std::string& thread_id, const std::string& bundle_json);
  Roe<void> MarkPskVerified(const std::string& thread_id);
  void RegisterPeerDirectEndpoint(const std::string& peer_account_id, const std::string& multiaddr);
  /** B28: best-first candidate list (RankAmpDialMultiaddrs order) via Amp RegisterEndpoints. */
  void RegisterPeerDirectEndpoints(const std::string& peer_account_id, const std::vector<std::string>& multiaddrs);
  /** Register all multiaddrs from a contact (keyed by its Account ID). */
  void RegisterContactDirectEndpoints(const Contact& contact);
  /** D052 — fetch one older-history page when scrolled to top. */
  void ScrollBackfill(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_complete = {});
  void TailSyncActiveE2eThread();
  void TickMesh();
  /** Warm connection for an open direct thread (background). */
  void WarmPeerForThread(const std::string& thread_id);
  /** Warm Amp association by PeerId / dial key (call prefetch shares this with chat). */
  void WarmPeerByKey(const std::string& peer_key);
  /** How a peer's warm mark comes off once it lapses (Amp ClearWarm). Set before AttachAmpTransports. */
  void SetClearPeerWarm(std::function<void(const std::string& peer_key)> clear) {
    transports_.SetClearWarm(std::move(clear));
  }
  /**
   * Send `control` (an identified system message) to `peer_identity` on the control lane
   * (kRelayRouteControl): a copy per device on the peer's card, each E2E under that device pair's e2e_public
   * session (the account copy for a peer with no card, M024), not sequenced, never stored. Queued on the critical lane — the relay first (retried
   * while fresh), else Amp. Our card rides it when the peer may not hold it; `with_card` when the caller knows it
   * may hold none of ours (ControlLane::Send).
   */
  Roe<void> SendControl(const std::string& peer_identity, const ThreadMessage& control, bool urgent = false,
                        bool with_card = false);
  /** Each of `sends` on the control lane, sealed per pair and in parallel off this thread (ControlLane::SendToMany). */
  void SendControlToMany(std::vector<ControlSend> sends, ControlSendsDone done = {},
                         WorkerLane lane = WorkerLane::Critical);
  /**
   * Our card to each of `accounts` on the control lane (the `device_list` notice, as to our siblings): a device
   * they do not know yet — one restored from a recovery phrase — is on their list before they next write.
   */
  void NotifyContactsOfDevices(const std::vector<std::string>& accounts);
  /** Snapshotted link UX for the open thread (header + soft banner). */
  ThreadPeerLinkView GetThreadPeerLink(const std::string& thread_id) const;
  /** Clear dial backoff and dial again for the thread's peer. */
  void RetryPeerDial(const std::string& thread_id);

private:
  /** A message to or from a peer over its link: its warm mark lasts (PeerWarmMarks::Touch). */
  void NoteMessageActivity(const std::string& thread_id, const std::string& device_peer_id);
  TrustLevel ResolveThreadTrust(const Thread& thread) const;
  /** The direct-chat transport is attached and reaches `peer_key` now. */
  bool AmpReaches(const std::string& peer_key) const;
  /** The peer's account KEM public key (directory, M015). */
  Roe<ByteVector> PeerKemPublicKey(const PeerSessionKey& key) const;
  void NotifyDeliveryIssue(const Thread& thread, const std::string& message_id, SendFailure failure,
                           const std::string& detail);
  void MaybeSurfaceReceiveFailure(const RelayReceiveOutcome& outcome);
  void MaybeEnqueueAttachmentDownload(const RelayReceiveOutcome& outcome);
  void ApplySendSucceeded(const std::string& thread_id, const std::string& message_id, MessageTransport transport);
  /** `detail`: the transport's own text, for the log. */
  void ApplySendFailed(const std::string& thread_id, const std::string& message_id, SendFailure failure,
                       const std::string& detail);
  /** A pair whose session came back without this device's numbering starts its next epoch (D014, #378). */
  Roe<void> StartNextEpochIfRestored(const Thread& thread);
  /** A pair this device lost its numbering in starts its next epoch (D014): 1:1 sends and group copies. */
  Roe<void> StartNextPairEpochIfRestored(const PeerSessionKey& key);
  /** One member's copy of a group message: that member's reachability, and the row once any copy is out. */
  void ApplyGroupMemberOutcome(const DeliveryOutcome& outcome);
  void MarkGroupMemberMissed(const std::string& group_id, const std::string& member);
  void NotifyGroupMembersMissed(const std::string& thread_id, const std::string& message_id);
  /** The message row's delivery state; false when there is no row (a control-lane send). */
  bool MarkDelivery(const std::string& thread_id, const std::string& message_id, bool success,
                    MessageTransport transport, bool unless_delivered = false);
  void MaybeRepairGap(const std::string& thread_id, const RelayEnvelope& envelope);
  void RunSyncOnIo(const std::string& thread_id, std::function<void(std::function<void(Roe<ChatSyncResult>)>)> task,
                   std::function<void(Roe<ChatSyncResult>)> on_complete);
  bool IsE2ePrivateThread(const std::string& thread_id) const;
  bool IsThreadCompromised(const std::string& thread_id) const;
  bool IsPskReadyToSend(const std::string& thread_id) const;
  /** A family's live exchange (a call): no key rotation, no background history sync meanwhile. */
  std::optional<std::string> LiveExchange() const;
  Roe<void> MaybeSendPublicAutoRekey(const std::string& thread_id);
  Roe<ThreadMessage> SendPublicPskRotate(const std::string& thread_id, PublicPskRotateKind kind);
  void PurgeRetryQueueForThread(const std::string& thread_id);
  void HandleDirectInbound(RelayEnvelope envelope, const std::string& from_peer_id);
  void LoadPersistedRelayCursor(const std::string& account_id);
  void PersistRelayCursor(const std::string& account_id);
  /** The relay refused a stranger's send to `recipient` for its price (MR005): the chat shows it (P001). */
  void NotePaymentRequired(const std::string& recipient, int64_t floor);
  /** After an inbound envelope: a verified sender is online (its grants, MR005). */
  void NoteVerifiedSender(const RelayEnvelope& envelope, bool sender_verified);

  static constexpr int64_t kReceiveFailureNoticeCooldownMs = 5 * 60 * 1000;

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IRelayClient* relay_ = nullptr;
  InboxController& inbox_;
  AccountKeyResolver& account_keys_;
  IPeerSessionStore& psk_store_;
  IGroupRosterStore& group_roster_;
  GroupMembershipWorkflow* groups_ = nullptr;
  InitiationBillingStore* initiation_billing_ = nullptr;
  AttachmentFetchWorkflow* attachment_downloads_ = nullptr;
  std::string profile_data_dir_;
  /** Control-lane ids applied (profile.db); declared before the pipeline that reads it. */
  IControlLaneSeenStore* control_lane_seen_ = nullptr;
  std::unique_ptr<RelayReceivePipeline> receive_pipeline_;
  /** The mesh transports (direct chat, history, blobs) and the peers' dial endpoints; after the
   *  pipeline their inbound handler feeds, so they go first. */
  PeerTransports transports_;
  /** Amp-or-relay sends and their retries; outcomes come back to ApplySend* on a worker. */
  DeliveryOutbox outbox_;
  std::unique_ptr<ChatSyncWorkflow> chat_sync_;
  EpochBumpCoordinator epoch_coordinator_;
  PskSessionCoordinator psk_coordinator_;
  PublicPskLockCoordinator public_lock_;
  const SystemControlRegistry* controls_ = nullptr;
  std::string support_account_id_;
  std::string relay_cursor_;
  std::function<void()> on_messages_changed_;
  std::function<void(const DeliveryNotice&)> on_delivery_notice_;
  std::function<void(std::string, std::string, std::string, bool)> on_background_unread_;
  std::function<void(std::string, std::string)> on_inbound_message_;
  InboundThreadIndex inbound_threads_;
  /**
   * Remembers where a stored inbound message went (and the call, when it is a call summary); returns the ids
   * to announce through `on_inbound_message_`. Independent of whether that callback is set yet.
   */
  std::vector<std::string> NoteInbound(const std::string& message_id, const RelayReceiveOutcome& outcome);
  /** Whether a stored inbound message in `thread_id` mentions us (G015). */
  /** A persisted message's notice for a user who is away (not attentive — the open chat too); none otherwise. */
  struct BackgroundNotice {
    std::string title;
    std::string body;
    std::string thread_id;
    bool mentions_us = false;
  };
  std::optional<BackgroundNotice> BackgroundNoticeFor(const RelayReceiveOutcome& outcome,
                                                      const std::string& local_account);
  /**
   * A group event about this account (removed; its accept met a full group) arrived in a direct message that stays
   * quiet: the GROUP thread counts it unread, and the user away gets a notice for it.
   */
  std::optional<BackgroundNotice> NoteEventAboutUs(const RelayReceiveOutcome& outcome);
  /**
   * G016: a group dissolved. Its chat gets the one unread count / preview, and the notice for a user who is away;
   * the DM that carried the news gets none. None when the outcome dissolved no group.
   */
  std::optional<BackgroundNotice> NoticeGroupDissolved(const RelayReceiveOutcome& outcome);
  std::optional<BackgroundNotice> NoticeGroupDissolvedIn(const std::string& thread_id);
  /** A live message that mentions us is marked (unless its chat is open); one that cannot be decided is held. */
  void NoteMention(const RelayReceiveOutcome& outcome, const std::string& local_account);
  struct HeldMention {
    std::string group_id;
    std::string thread_id;
    ThreadMessage message;
    std::string local_account;
    int64_t held_at_ms = 0;
  };
  IThreadMentionsStore* mentions_ = nullptr;
  /** How long a suspected missing copy waits for the message to come another way before it is asked for. */
  static constexpr int64_t kMissingCopyWaitMs = 20 * 1000;
  std::mutex asked_copies_mutex_;
  std::set<std::string> asked_copies_; // message ids asked for (bounded)
  /**
   * M021 d6: an envelope said its sender's list is newer than ours, or could not be checked for want of a card — ask
   * that device for it, through the relay, when card_ask_gate_ allows (an unverified envelope's word is rate-limited).
   */
  void AskForNewerCard(const std::string& account, const std::string& device, int64_t issued_at,
                       CardAskGate::Source source);
  std::mutex card_asks_mutex_;
  CardAskGate card_ask_gate_; // under card_asks_mutex_
  /**
   * M024: a verified lane message from `account` this device could not read (no copy for it, or an account copy
   * that did not open) — tell it our card (`device_list`), so its next message has a copy for this device. At most
   * once per account per kShowCardGapMs. The message itself is not asked for: signaling is stale by then.
   */
  void ShowCardTo(const std::string& account);
  /**
   * #555: a card of `account`'s may be kept now (a verified envelope of its, a roster's card): the lane messages of
   * its held for one (RelayReceivePipeline::ReplayHeldControl) are checked again on a worker, and followed up as
   * on arrival.
   */
  void ReplayHeldControl(const std::string& account);
  static constexpr int64_t kShowCardGapMs = 10 * 60 * 1000;
  std::map<std::string, int64_t> cards_shown_; // account → when last told (under card_asks_mutex_)
  Roe<void> ResendDirectCopy(const Thread& thread, const ThreadMessage& message, const std::string& asker,
                             const AccountCardDevice& device);
  Roe<void> ResendGroupCopy(const Thread& thread, const ThreadMessage& message, const std::string& asker,
                            const AccountCardDevice& device);
  std::mutex held_mentions_mutex_;
  std::vector<HeldMention> held_mentions_;
  bool InboundMentionsUs(const std::string& thread_id, const ThreadMessage& message, const std::string& local_account);
  mutable std::mutex receive_failure_mutex_;
  std::unordered_map<std::string, int64_t> receive_failure_last_ms_;
  /** Our key card for an envelope carrying a key_init (none when the identity is not ready). */
  /** Our account card as envelopes carry it (with a key_init); none when it cannot be issued. */
  std::optional<Object> OwnCardJson();
  /** Who we are on the envelopes we send (this device signs them). */
  Roe<EnvelopeSender> OwnSender(const LocalIdentity& identity);
  /** The peer's devices on its newest kept card (M021); empty while we know none (an account copy then). */
  std::vector<AccountCardDevice> PeerDevices(const std::string& account_id);
  /** This device's signature on an envelope. */
  Roe<void> SignEnvelope(RelayEnvelope& envelope);
  /** Our card to our other devices, through our own mailbox (M022). */
  void NotifySiblings();
  /** The issue time of the own card our siblings were last told (a mesh bring-up does not repeat it). */
  std::atomic<int64_t> siblings_told_card_at_{0};
  /** A link to device `peer_id` is up (its copy goes over it, M022). */
  bool LinkedTo(const std::string& peer_id) const;
  /** The pair's e2e_public sessions (the control lane, call media-key wraps). */
  E2ePublicSessionKeys session_keys_;
  /** Our cards: the key card carried with every key_init, the account card served to who asks. */
  OwnCards own_cards_;
  std::function<void()> on_sibling_notice_;
  std::function<void()> on_own_devices_changed_;
  AccountCardService card_service_;
  /** Identified system messages to a peer (call signaling): E2E, unsequenced, never stored. */
  ControlLane control_lane_;
  /** Relay grants issued and held (MR005); after the control lane, which carries them. */
  RelayGrantCoordinator relay_grants_;
  uint64_t last_relay_poll_ms_ = 0;
  /** 0 = Unknown, 1 = Ok, 2 = Failed — see BriefRelayHealthState. */
  std::atomic<int> brief_relay_health_{0};
  void StoreBriefRelayHealth(BriefRelayHealthState state);
  std::mutex brief_relay_health_cb_mu_;
  std::function<void(BriefRelayHealthState)> on_brief_relay_health_changed_;
  std::atomic<bool> poll_pending_{false};
  /** Set when SyncInbox is requested while a poll is already in flight — worker re-polls. */
  std::atomic<bool> poll_again_{false};
  std::atomic<bool> sync_pending_{false};
};

} // namespace pbr
