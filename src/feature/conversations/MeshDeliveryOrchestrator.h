#pragma once

#include "mesh/host/DisclosureGatedPeerLinks.h"
#include "common/Module.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "common/thread/IThreadStore.h"
#include "lib/pricing/InitiationBillingStore.h"
#include "lib/pricing/PaymentPromiseStore.h"
#include "lib/messaging/DeliveryNotice.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/PeerKemKeyStore.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "lib/messaging/PeerSigningKeyStore.h"
#include "lib/messaging/SendRelayOptions.h"
#include "feature/conversations/ChatSyncWorkflow.h"
#include "domain/peers/AccountKeyResolver.h"
#include "domain/peers/OwnCards.h"
#include "lib/messaging/OutboundMessage.h"
#include "domain/delivery/AccountCardService.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "lib/messaging/EpochBumpCoordinator.h"
#include "feature/conversations/InboxController.h"
#include "common/chat/IDirectMessageClient.h"
#include "lib/messaging/PskSessionCoordinator.h"
#include "lib/messaging/PublicPskLockCoordinator.h"
#include "lib/messaging/E2ePublicSessionLogic.h"
#include "common/chat/SystemControl.h"
#include "lib/messaging/ControlLaneSeenStore.h"
#include "domain/delivery/PeerRouteBook.h"
#include "domain/delivery/ControlLane.h"
#include "domain/delivery/DeliveryOutbox.h"
#include "domain/delivery/E2ePublicSessionKeys.h"
#include "domain/delivery/PeerTransports.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/GroupInviteGate.h"
#include "mesh/host/MeshPorts.h"
#include "lib/net/OrgBackendClients.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
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
  Direct,      // live ADP PeerLink
  ViaHop,      // live circuit/media carrier PeerLink
  ViaRelay,    // Brief/HTTP message relay only (no live mesh link)
  Connecting,  // dial / handshake / punch — short-lived
  Degraded,    // backoff / retry soon
  Failed,      // offline / can't connect
  Ready,       // idle, not yet dialing
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
  /** Messages can go via the Brief relay (a route to the peer is known and a relay is configured). */
  bool relay_available = false;
  bool carrier_backed = false;
};

class MeshDeliveryOrchestrator : public Module {
public:
  /** Amp single entry for chat / history / blob ([A020]); requires `amp_links`. */
  MeshDeliveryOrchestrator(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity, IRelayClient* relay,
                      InboxController& inbox, AccountKeyResolver& account_keys, IPskSessionStore& psk_store,
                      GroupRosterStore& group_roster, GroupInviteGate* invite_gate = nullptr,
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
  Roe<void> SendChargeRequired(const std::string& peer_identity,
                               std::optional<int64_t> floor_minor = std::nullopt);
  InitiationBillingStore* InitiationBilling() const { return initiation_billing_; }
  /** D098 — append reaction / reaction_clear annotation on direct or group thread. */
  Roe<ThreadMessage> SendReaction(const std::string& thread_id, const std::string& target_message_id,
                                  const std::string& emoji);
  /** "Delete for everyone": tell the peer / group to delete our message `target_message_id`. */
  Roe<ThreadMessage> SendRetract(const std::string& thread_id, const std::string& target_message_id);
  /** A deleted message that has not gone out yet is not retried. */
  void CancelPendingSend(const std::string& message_id);
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
  /** Me recovery: delete undelivered relay rows older than `older_than_days` (does not clear local chat). */
  Roe<RelayDeleteResult> ClearUndeliveredOlderThan(int older_than_days);
  void RetryFailedOutbound();
  void SetRelayClient(IRelayClient* relay);
  /** The system-control families features bind (call control, …); passed on to the receive path. Not owned. */
  void SetSystemControls(const SystemControlRegistry* controls);
  void SetGroupMembership(GroupMembershipWorkflow* groups);
  void SetInitiationBillingStore(InitiationBillingStore* store);
  void SetProfileDataDir(std::string profile_data_dir);
  void SetAttachmentDownloads(AttachmentFetchWorkflow* downloads);
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
  /** Where an account is reached on the relay, from what is known here (no lookup). */
  std::optional<std::string> RelayRouteOf(const std::string& account_id) const;
  /** Account cards between devices (the card exchange over Amp). */
  AccountCardService& AccountCards() { return card_service_; }
  /** This account's own key card and device list (M021). */
  OwnCards& OwnAccountCards() { return own_cards_; }
  /** Our device list changed (our address goes to our other devices too, M021). */
  void SetOnOwnDevicesChanged(std::function<void()> changed) { on_own_devices_changed_ = std::move(changed); }
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
  void RegisterPeerDirectEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);
  /** B28: best-first candidate list (RankAmpDialMultiaddrs order) via Amp RegisterEndpoints. */
  void RegisterPeerDirectEndpoints(const std::string& peer_relay_user_id,
                                   const std::vector<std::string>& multiaddrs);
  /** Register all multiaddrs from a contact (keyed by relay id). */
  void RegisterContactDirectEndpoints(const Contact& contact);
  /**
   * Brief route sources for Account→`relay:` (P001 strangers / non-contact calls).
   * Shadows: cached directory hits; directory: sync LookupByAccount on send miss.
   */
  void SetPeerRouteSources(DirectoryShadowCache* shadows, mesh::IDirectoryClient* directory);
  /** Learn Account→`relay:` from inbound `sender_contact_id` + `sender_relay_id`. */
  void NoteAccountRelayRoute(const std::string& account_id, const std::string& relay_user_id);
  /** D052 — fetch one older-history page when scrolled to top. */
  void ScrollBackfill(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_complete = {});
  void TailSyncActiveE2eThread();
  void TickMesh();
  /** Warm connection for an open direct thread (background). */
  void WarmPeerForThread(const std::string& thread_id);
  /** Warm Amp association by PeerId / dial key (call prefetch shares this with chat). */
  void WarmPeerByKey(const std::string& peer_key);
  /**
   * Ensure e2e_public AutoKey PSK exists for peer (call invite media-key wrap).
   * Caller attaches `first_message_key_init_b64` on the outbound CallInvite send.
   */
  Roe<EnsuredE2ePublicSessionKey> EnsureE2ePublicSessionKey(const std::string& peer_identity);
  /**
   * Send `control` (an identified system message) to `peer_identity` on the control lane
   * (kRelayRouteControl): E2E under the pair's e2e_public session (opened on first use; a
   * `key_init_b64` the caller holds rides along), not sequenced, never stored. Queued on the
   * critical lane — relay when a route is known (retried while fresh), else Amp.
   */
  Roe<void> SendControl(const std::string& peer_identity, const ThreadMessage& control,
                        const std::optional<std::string>& key_init_b64 = std::nullopt, bool urgent = false);
  /**
   * Our card to each of `accounts` on the control lane (the `device_list` notice, as to our siblings): a device
   * they do not know yet — one restored from the account backup — is on their list before they next write.
   */
  void NotifyContactsOfDevices(const std::vector<std::string>& accounts);
  /** Snapshotted link UX for the open thread (header + soft banner). */
  ThreadPeerLinkView GetThreadPeerLink(const std::string& thread_id) const;
  /** Clear dial backoff and dial again for the thread's peer. */
  void RetryPeerDial(const std::string& thread_id);

private:

  TrustLevel ResolveThreadTrust(const Thread& thread) const;
  /** The direct-chat transport is attached and reaches `peer_key` now. */
  bool AmpReaches(const std::string& peer_key) const;
  /** The peer's account KEM public key (directory, M015). */
  Roe<ByteVector> PeerKemPublicKey(const ChatTargetKey& key) const;
  void NotifyDeliveryIssue(const Thread& thread, SendFailure failure, const std::string& detail);
  void NotifyRelayFallback(const std::string& thread_id);
  void MaybeSurfaceReceiveFailure(const RelayReceiveOutcome& outcome);
  void MaybeEnqueueAttachmentDownload(const RelayReceiveOutcome& outcome);
  void ApplySendSucceeded(const std::string& thread_id, const std::string& message_id, MessageTransport transport,
                          bool relay_after_direct_attempt = false);
  /** `detail`: the transport's own text, for the log. */
  void ApplySendFailed(const std::string& thread_id, const std::string& message_id, SendFailure failure,
                       const std::string& detail);
  /** A pair whose session came back without this device's numbering starts its next epoch (D014, #378). */
  Roe<void> StartNextEpochIfRestored(const Thread& thread);
  /** A pair this device lost its numbering in starts its next epoch (D014): 1:1 sends and group copies. */
  Roe<void> StartNextPairEpochIfRestored(const ChatTargetKey& key);
  /** One member's copy of a group message: that member's reachability, and the row once any copy is out. */
  void ApplyGroupMemberOutcome(const DeliveryOutcome& outcome);
  void MarkGroupMemberMissed(const std::string& group_id, const std::string& member);
  void NotifyGroupMembersMissed();
  /** The message row's delivery state; false when there is no row (a control-lane send). */
  bool MarkDelivery(const std::string& thread_id, const std::string& message_id, bool success, MessageTransport transport,
                    bool unless_delivered = false);
  void MaybeRepairGap(const std::string& thread_id, const RelayEnvelope& envelope);
  void RunSyncOnIo(const std::string& thread_id,
                   std::function<void(std::function<void(Roe<ChatSyncResult>)>)> task,
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
  void LoadPersistedRelayCursor(const std::string& relay_user_id);
  void PersistRelayCursor(const std::string& relay_user_id);

  static constexpr int64_t kReceiveFailureNoticeCooldownMs = 5 * 60 * 1000;

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IRelayClient* relay_ = nullptr;
  InboxController& inbox_;
  AccountKeyResolver& account_keys_;
  IPskSessionStore& psk_store_;
  GroupRosterStore& group_roster_;
  GroupMembershipWorkflow* groups_ = nullptr;
  InitiationBillingStore* initiation_billing_ = nullptr;
  AttachmentFetchWorkflow* attachment_downloads_ = nullptr;
  std::string profile_data_dir_;
  /** Control-lane ids applied (profile.db); declared before the pipeline that reads it. */
  std::unique_ptr<ControlLaneSeenStore> control_lane_seen_;
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
  /** Whether a stored inbound message in `thread_id` mentions us (G015). */
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
  /** Where each peer is reached on the relay (contacts, learned routes, the directory). */
  PeerRouteBook routes_;
  /** The pair's e2e_public sessions (the control lane, call media-key wraps). */
  E2ePublicSessionKeys session_keys_;
  /** Our cards: the key card carried with every key_init, the account card served to who asks. */
  OwnCards own_cards_;
  std::function<void()> on_own_devices_changed_;
  AccountCardService card_service_;
  /** Identified system messages to a peer (call signaling): E2E, unsequenced, never stored. */
  ControlLane control_lane_;
  uint64_t last_relay_poll_ms_ = 0;
  /** 0 = Unknown, 1 = Ok, 2 = Failed — see BriefRelayHealthState. */
  std::atomic<int> brief_relay_health_{0};
  std::atomic<bool> poll_pending_{false};
  /** Set when SyncInbox is requested while a poll is already in flight — worker re-polls. */
  std::atomic<bool> poll_again_{false};
  std::atomic<bool> sync_pending_{false};
};

} // namespace pbr
