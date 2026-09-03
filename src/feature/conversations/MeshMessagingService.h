#pragma once

#include "common/Module.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "common/thread/IThreadStore.h"
#include "domain/messaging/InitiationBillingStore.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/PeerKemKeyStore.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "domain/messaging/PeerSigningKeyStore.h"
#include "domain/messaging/SendRelayOptions.h"
#include "feature/conversations/ChatSyncService.h"
#include "feature/conversations/DirectoryShadowCache.h"
#include "domain/messaging/EpochBumpCoordinator.h"
#include "feature/conversations/InboxController.h"
#include "common/chat/IDirectMessageClient.h"
#include "domain/messaging/PskSessionCoordinator.h"
#include "domain/messaging/PublicPskLockCoordinator.h"
#include "feature/conversations/RelayReceivePipeline.h"
#include "feature/conversations/GroupInviteGate.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/net/ServiceClients.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class CallSessionManager;
class GroupMembershipService;
class AttachmentDownloadService;
class IChatHistoryPeerClient;
class IChatBlobPeerClient;

/** Aggregated peer-link UX for a direct chat thread. */
struct ThreadPeerLinkView {
  MeshPeerLinkPhase phase = MeshPeerLinkPhase::Unavailable;
  std::string status_label;
  std::string banner_message;
  bool show_banner = false;
  bool show_retry = false;
  int backoff_seconds = 0;
  bool has_direct_endpoint = false;
  bool relay_available = false;
};

class MeshMessagingService : public Module {
public:
  /** Amp single entry for chat / history / blob ([A020]); requires `amp_links`. */
  MeshMessagingService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity, IRelayClient* relay,
                      InboxController& inbox, PeerSigningKeyStore& signing_key_store,
                      IPeerSigningKeyResolver& signing_key_resolver, PeerKemKeyStore& kem_key_store,
                      IPeerKemKeyResolver& kem_key_resolver, IPskSessionStore& psk_store,
                      GroupRosterStore& group_roster, GroupInviteGate* invite_gate = nullptr,
                      IChatPeerLinks* amp_links = nullptr, std::function<void()> amp_io_pump = {},
                      std::function<void(std::function<void()>)> amp_worker_post = {});

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
  void SetCallSessionManager(CallSessionManager* calls);
  void SetGroupMembership(GroupMembershipService* groups);
  void SetInitiationBillingStore(InitiationBillingStore* store);
  void SetProfileDataDir(std::string profile_data_dir);
  void SetAttachmentDownloads(AttachmentDownloadService* downloads);
  /** R019 peer-direct attachment blobs (null when mesh unavailable). */
  IChatBlobPeerClient* PeerBlobClient() const;
  IChatBlobPeerService* PeerBlobService() const;
  void SetOnMessagesChanged(std::function<void()> callback);
  void NotifyMessagesChanged();
  void SetOnDeliveryNotice(std::function<void(const std::string&)> callback);
  /** Fired on UI thread when a background ingest bumps unread (for OS notify). */
  void SetOnBackgroundUnread(
      std::function<void(std::string title, std::string body, std::string thread_id)> callback);
  void RegisterPeerSigningKey(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                              const std::string& signing_public_key_b64, const std::string& source = "manual");
  void RegisterPeerKemKey(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                          const std::string& kem_public_key_b64, const std::string& source = "manual");
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
  /** Register all multiaddrs from a contact (keyed by relay id). */
  void RegisterContactDirectEndpoints(const Contact& contact);
  /**
   * Brief route sources for Account→`relay:` (P001 strangers / non-contact calls).
   * Shadows: cached directory hits; directory: sync LookupByAccount on send miss.
   */
  void SetPeerRouteSources(DirectoryShadowCache* shadows, IDirectoryClient* directory);
  /** Learn Account→`relay:` from inbound `sender_contact_id` + `sender_relay_id`. */
  void NoteAccountRelayRoute(const std::string& account_id, const std::string& relay_user_id);
  /** D052 — fetch one older-history page when scrolled to top. */
  void ScrollBackfill(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_complete = {});
  void TailSyncActiveE2eThread();
  void TickMesh();
  /** Warm connection for an open direct thread (background). */
  void WarmPeerForThread(const std::string& thread_id);
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
  void RememberRouteFromEnvelope(const RelayEnvelope& envelope);
  TrustLevel ResolveThreadTrust(const Thread& thread) const;
  void EnqueueRetry(PendingRelaySend pending);
  void NotifyDeliveryIssue(const Thread& thread, const std::string& error_message);
  void NotifyRelayFallback(const std::string& thread_id);
  void MaybeSurfaceReceiveFailure(const RelayReceiveOutcome& outcome);
  void MaybeEnqueueAttachmentDownload(const RelayEnvelope& envelope, const std::string& thread_id);
  void ApplySendResult(const std::string& thread_id, const std::string& message_id, bool success,
                       const std::string& error_message = {},
                       MessageTransport transport = MessageTransport::Relay,
                       bool relay_after_direct_attempt = false);
  void RegisterMockPeerKeyForReply(const std::string& peer_identity_value);
  void MaybeRepairGap(const std::string& thread_id, const RelayEnvelope& envelope);
  void RunSyncOnIo(const std::string& thread_id, std::function<Roe<ChatSyncResult>()> task,
                   std::function<void(Roe<ChatSyncResult>)> on_complete);
  bool IsE2ePrivateThread(const std::string& thread_id) const;
  bool IsThreadCompromised(const std::string& thread_id) const;
  bool IsPskReadyToSend(const std::string& thread_id) const;
  bool HasActiveLocalCall() const;
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
  GroupMembershipService* groups_ = nullptr;
  InitiationBillingStore* initiation_billing_ = nullptr;
  AttachmentDownloadService* attachment_downloads_ = nullptr;
  std::string profile_data_dir_;
  IChatPeerLinks* amp_links_ = nullptr;
  std::unique_ptr<RelayReceivePipeline> receive_pipeline_;
  std::unique_ptr<IChatHistoryPeerClient> peer_history_;
  std::unique_ptr<IChatBlobPeerService> peer_blob_;
  std::unique_ptr<IDirectMessageClient> direct_chat_;
  std::unique_ptr<ChatSyncService> chat_sync_;
  EpochBumpCoordinator epoch_coordinator_;
  PskSessionCoordinator psk_coordinator_;
  PublicPskLockCoordinator public_lock_;
  CallSessionManager* call_sessions_ = nullptr;
  DirectoryShadowCache* directory_shadows_ = nullptr;
  IDirectoryClient* directory_ = nullptr;
  std::string support_account_id_;
  std::string relay_cursor_;
  std::function<void()> on_messages_changed_;
  std::function<void(const std::string&)> on_delivery_notice_;
  std::function<void(std::string, std::string, std::string)> on_background_unread_;
  mutable std::mutex retry_mutex_;
  std::vector<PendingRelaySend> retry_queue_;
  mutable std::mutex link_ux_mutex_;
  mutable std::string relay_fallback_notice_thread_id_;
  mutable std::string relay_fallback_notice_text_;
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
