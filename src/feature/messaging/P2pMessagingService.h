#pragma once

#include "common/Module.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/PeerKemKeyStore.h"
#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "base/messaging/SendRelayOptions.h"
#include "feature/messaging/ChatSyncService.h"
#include "feature/messaging/EpochBumpCoordinator.h"
#include "feature/messaging/InboxController.h"
#include "feature/messaging/Libp2pChatHistoryService.h"
#include "feature/messaging/Libp2pDirectChatService.h"
#include "feature/messaging/PskSessionCoordinator.h"
#include "feature/messaging/RelayReceivePipeline.h"
#include "feature/messaging/GroupInviteGate.h"
#include "base/net/ServiceClients.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

class CallSessionManager;
class GroupMembershipService;

/** Aggregated peer-link UX for a direct chat thread. */
struct ThreadPeerLinkView {
  PeerLinkPhase phase = PeerLinkPhase::Unavailable;
  std::string status_label;
  std::string banner_message;
  bool show_banner = false;
  bool show_retry = false;
  int backoff_seconds = 0;
  bool has_direct_endpoint = false;
  bool relay_available = false;
};

class P2pMessagingService : public Module {
public:
  P2pMessagingService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity, IRelayClient* relay,
                      InboxController& inbox, PeerSigningKeyStore& signing_key_store,
                      IPeerSigningKeyResolver& signing_key_resolver, PeerKemKeyStore& kem_key_store,
                                         IPeerKemKeyResolver& kem_key_resolver, IPskSessionStore& psk_store,
                                         GroupRosterStore& group_roster, GroupInviteGate* invite_gate = nullptr,
                                         Libp2pHost* libp2p_host = nullptr, PeerSessionManager* peer_sessions = nullptr);

  Roe<ThreadMessage> SendUserMessage(const std::string& thread_id, const std::string& text,
                                     const SendRelayOptions& options = {});
  Roe<ThreadMessage> SendGroupMessage(const std::string& thread_id, const std::string& text,
                                      const SendRelayOptions& options = {});
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
  void SetProfileDataDir(std::string profile_data_dir);
  void SetOnMessagesChanged(std::function<void()> callback);
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
  /** D052 — fetch one older-history page when scrolled to top. */
  void ScrollBackfill(const std::string& thread_id, std::function<void(Roe<ChatSyncResult>)> on_complete = {});
  void TailSyncActiveE2eThread();
  void TickLibp2p();
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
  TrustLevel ResolveThreadTrust(const Thread& thread) const;
  void EnqueueRetry(PendingRelaySend pending);
  void NotifyDeliveryIssue(const Thread& thread, const std::string& error_message);
  void NotifyRelayFallback(const std::string& thread_id);
  void MaybeSurfaceReceiveFailure(const RelayReceiveOutcome& outcome);
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
  std::string profile_data_dir_;
  Libp2pHost* libp2p_host_ = nullptr;
  PeerSessionManager* peer_sessions_ = nullptr;
  std::unique_ptr<RelayReceivePipeline> receive_pipeline_;
  std::unique_ptr<Libp2pChatHistoryService> peer_history_;
  std::unique_ptr<Libp2pDirectChatService> direct_chat_;
  std::unique_ptr<ChatSyncService> chat_sync_;
  EpochBumpCoordinator epoch_coordinator_;
  PskSessionCoordinator psk_coordinator_;
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
  uint64_t last_relay_poll_ms_ = 0;
  /** 0 = Unknown, 1 = Ok, 2 = Failed — see BriefRelayHealthState. */
  std::atomic<int> brief_relay_health_{0};
  std::atomic<bool> poll_pending_{false};
  /** Set when SyncInbox is requested while a poll is already in flight — worker re-polls. */
  std::atomic<bool> poll_again_{false};
  std::atomic<bool> sync_pending_{false};
};

} // namespace pbr
