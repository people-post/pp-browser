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
#include "domain/peers/DirectoryShadowCache.h"
#include "lib/messaging/EpochBumpCoordinator.h"
#include "feature/conversations/InboxController.h"
#include "common/chat/IDirectMessageClient.h"
#include "lib/messaging/PskSessionCoordinator.h"
#include "lib/messaging/PublicPskLockCoordinator.h"
#include "lib/messaging/E2ePublicSessionLogic.h"
#include "common/chat/SystemControl.h"
#include "lib/messaging/ControlLaneSeenStore.h"
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
                      InboxController& inbox, PeerSigningKeyStore& signing_key_store,
                      IPeerSigningKeyResolver& signing_key_resolver, PeerKemKeyStore& kem_key_store,
                      IPeerKemKeyResolver& kem_key_resolver, IPskSessionStore& psk_store,
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
  void SetAddressDisclosure(const mesh::AddressDisclosureGate* gate) { address_disclosure_ = gate; }
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
  /** Fired on UI thread when a background ingest bumps unread (for OS notify). */
  void SetOnBackgroundUnread(
      std::function<void(std::string title, std::string body, std::string thread_id)> callback);
  void RegisterPeerSigningKey(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                              const std::string& signing_public_key_b64, const std::string& source = "manual");
  void RegisterPeerKemKey(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                          const std::string& kem_public_key_b64, const std::string& source = "manual");

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
  /** Snapshotted link UX for the open thread (header + soft banner). */
  ThreadPeerLinkView GetThreadPeerLink(const std::string& thread_id) const;
  /** Clear dial backoff and dial again for the thread's peer. */
  void RetryPeerDial(const std::string& thread_id);

private:
  struct PendingRelaySend {
    RelayEnvelope envelope;
    std::string message_id;
    std::string thread_id;
    int attempt_count = 0;
  };

  std::optional<std::string> ResolvePeerRelayId(const Thread& thread) const;
  /** Sync directory LookupByAccount when contacts/learned/shadow miss (send path). */
  std::optional<std::string> ResolvePeerRelayIdWithDirectory(const Thread& thread);
  void RememberRouteFromEnvelope(const RelayEnvelope& envelope, const RelayReceiveOutcome& outcome);
  TrustLevel ResolveThreadTrust(const Thread& thread) const;
  /** The pair's e2e_public session as the lane and media-key wraps use it. */
  struct E2ePublicSession {
    ChatTargetKey target_key;
    uint32_t epoch = 1;
    ByteVector master_psk;
    /** Set when this call just opened the session (attach on the first send). */
    std::optional<std::string> key_init_b64;
  };
  Roe<E2ePublicSession> ResolveE2ePublicSession(const std::string& peer_identity);
  /** The peer's account KEM public key (directory, M015). */
  Roe<ByteVector> PeerKemPublicKey(const ChatTargetKey& key) const;
  uint64_t NextControlOrderKey();
  void EnqueueRetry(PendingRelaySend pending);
  void NotifyDeliveryIssue(const Thread& thread, SendFailure failure, const std::string& detail);
  void NotifyRelayFallback(const std::string& thread_id);
  void MaybeSurfaceReceiveFailure(const RelayReceiveOutcome& outcome);
  void MaybeEnqueueAttachmentDownload(const RelayReceiveOutcome& outcome);
  void ApplySendSucceeded(const std::string& thread_id, const std::string& message_id, MessageTransport transport,
                          bool relay_after_direct_attempt = false);
  /** `detail`: the transport's own text, for the log. */
  void ApplySendFailed(const std::string& thread_id, const std::string& message_id, SendFailure failure,
                       const std::string& detail);
  /** The message row's delivery state; false when there is no row (a control-lane send). */
  bool MarkDelivery(const std::string& thread_id, const std::string& message_id, bool success, MessageTransport transport);
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
  void HandleDirectInbound(RelayEnvelope envelope);
  void LoadPersistedRelayCursor(const std::string& relay_user_id);
  void PersistRelayCursor(const std::string& relay_user_id);

  static constexpr int64_t kReceiveFailureNoticeCooldownMs = 5 * 60 * 1000;

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IRelayClient* relay_ = nullptr;
  InboxController& inbox_;
  PeerSigningKeyStore& signing_key_store_;
  IPeerSigningKeyResolver& signing_key_resolver_;
  PeerKemKeyStore& kem_key_store_;
  IPeerKemKeyResolver& kem_key_resolver_;
  IPskSessionStore& psk_store_;
  GroupRosterStore& group_roster_;
  GroupMembershipWorkflow* groups_ = nullptr;
  InitiationBillingStore* initiation_billing_ = nullptr;
  AttachmentFetchWorkflow* attachment_downloads_ = nullptr;
  std::string profile_data_dir_;
  mesh::IChatPeerLinks* amp_links_ = nullptr;
  const mesh::AddressDisclosureGate* address_disclosure_ = nullptr;
  /** `amp_links_` limited to the audience, for the people-facing transports (outlives them). */
  std::unique_ptr<mesh::DisclosureGatedPeerLinks> people_links_;
  /** Control-lane ids applied (profile.db); declared before the pipeline that reads it. */
  std::unique_ptr<ControlLaneSeenStore> control_lane_seen_;
  std::unique_ptr<RelayReceivePipeline> receive_pipeline_;
  std::unique_ptr<IChatHistoryPeerClient> peer_history_;
  std::unique_ptr<IChatBlobPeerService> peer_blob_;
  std::unique_ptr<IDirectMessageClient> direct_chat_;
  std::unique_ptr<ChatSyncWorkflow> chat_sync_;
  EpochBumpCoordinator epoch_coordinator_;
  PskSessionCoordinator psk_coordinator_;
  PublicPskLockCoordinator public_lock_;
  const SystemControlRegistry* controls_ = nullptr;
  DirectoryShadowCache* directory_shadows_ = nullptr;
  mesh::IDirectoryClient* directory_ = nullptr;
  std::string support_account_id_;
  std::string relay_cursor_;
  std::function<void()> on_messages_changed_;
  std::function<void(const DeliveryNotice&)> on_delivery_notice_;
  std::function<void(std::string, std::string, std::string)> on_background_unread_;
  mutable std::mutex retry_mutex_;
  std::vector<PendingRelaySend> retry_queue_;
  std::atomic<uint64_t> control_order_key_{0};
  mutable std::mutex receive_failure_mutex_;
  std::unordered_map<std::string, int64_t> receive_failure_last_ms_;
  mutable std::mutex account_relay_mutex_;
  /** Inbound-learned Account ID → Brief `relay:` (non-contact call/message). */
  std::unordered_map<std::string, std::string> account_to_relay_;
  uint64_t last_relay_poll_ms_ = 0;
  /** 0 = Unknown, 1 = Ok, 2 = Failed — see BriefRelayHealthState. */
  std::atomic<int> brief_relay_health_{0};
  std::atomic<bool> poll_pending_{false};
  /** Set when SyncInbox is requested while a poll is already in flight — worker re-polls. */
  std::atomic<bool> poll_again_{false};
  std::atomic<bool> sync_pending_{false};
};

} // namespace pbr
