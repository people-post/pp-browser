#include "domain/people/ContactTypes.h"
#include "feature/conversations/GroupMembershipService.h"
#include "feature/conversations/AttachmentDownloadService.h"
#include "feature/conversations/AmpChatBlobService.h"
#include "feature/conversations/AmpChatHistoryService.h"
#include "feature/conversations/AmpDirectChatService.h"
#include "feature/conversations/MeshMessagingService.h"
#include "domain/messaging/PublicPskLockCoordinator.h"

#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/Utilities.h"
#include "domain/people/DirectChatTargetFromContact.h"
#include "domain/messaging/InitiationBillingCodec.h"
#include "domain/messaging/InitiationPricing.h"
#include "foundation/data/PricingTypes.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/messaging/ChatPayloadCodec.h"
#include "common/chat/ChatPayloadTypes.h"
#include "domain/messaging/E2eIntegrityUtil.h"
#include "domain/messaging/E2eRelayPayloadCodec.h"
#include "domain/messaging/GroupE2ePayloadCodec.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/EnvelopeSigner.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "domain/messaging/ReactionTypes.h"
#include "domain/messaging/PskRotateCodec.h"
#include "domain/messaging/SendRelayOptions.h"
#include "common/chat/MessagingLimits.h"
#include "common/EmojiKey.h"
#include "common/chat/RelayStreamKey.h"
#include "domain/messaging/RelayWirePayload.h"
#include "domain/people/PeerBriefRoute.h"
#include "common/thread/SyncStateTypes.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/net/ServiceClientsImpl.h"
#include "domain/net/RelayInboxCursor.h"
#include "domain/people/ContactIdentity.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "foundation/platform/os/OsTime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {
namespace {

Roe<void> EnsureAnnotationCap(IThreadStore& store, const std::string& thread_id,
                              const ThreadMessage& message) {
  if (message.content_type != ChatContentType::Annotation) {
    return {};
  }
  const std::string target_id = message.target_message_id.value_or("");
  if (target_id.empty()) {
    return Error("Annotation missing target_message_id");
  }
  auto count = store.CountAnnotationsForTarget(thread_id, target_id);
  if (!count) {
    return count.error();
  }
  if (static_cast<size_t>(*count) >= kMaxAnnotationsPerTarget) {
    return Error("Too many reactions on this message");
  }
  return {};
}

void ApplyRichPayloadOptions(ThreadMessage& message, const SendRelayOptions& options) {
  if (options.content_type) {
    message.content_type = *options.content_type;
  }
  if (options.payload_json) {
    message.payload_json = *options.payload_json;
  }
  if (message.content_type == ChatContentType::Annotation) {
    if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json)) {
      if (!fields->target_message_id.empty()) {
        message.target_message_id = fields->target_message_id;
      }
    }
  }
}

} // namespace

namespace {

constexpr const char* kMockPeerSigningPublicKeyB64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";

DirectChatTarget InboundTargetFromEnvelope(const RelayEnvelope& envelope) {
  DirectChatTarget target;
  target.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  target.peer_identity_value = envelope.sender_contact_id;
  target.channel = envelope.route.channel;
  return target;
}

} // namespace

MeshMessagingService::MeshMessagingService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                         IRelayClient* relay, InboxController& inbox,
                                         PeerSigningKeyStore& signing_key_store,
                                         IPeerSigningKeyResolver& signing_key_resolver, PeerKemKeyStore& kem_key_store,
                                         IPeerKemKeyResolver& kem_key_resolver, IPskSessionStore& psk_store,
                                         GroupRosterStore& group_roster, GroupInviteGate* invite_gate,
                                         IChatPeerLinks* amp_links, std::function<void()> amp_io_pump,
                                         std::function<void(std::function<void()>)> amp_worker_post)
    : store_(store), contacts_(contacts), identity_(identity), relay_(relay), inbox_(inbox),
      signing_key_store_(signing_key_store), signing_key_resolver_(signing_key_resolver), kem_key_store_(kem_key_store),
      kem_key_resolver_(kem_key_resolver), psk_store_(psk_store), group_roster_(group_roster), amp_links_(amp_links),
      epoch_coordinator_(store), psk_coordinator_(store, psk_store), public_lock_(store, psk_store) {
  redirectLogger("MeshMessagingService");
  receive_pipeline_ =
      std::make_unique<RelayReceivePipeline>(store_, signing_key_resolver_, psk_store_, identity_, group_roster_,
                                             invite_gate);
  // Blob + chat/history: Amp single entry ([A020] / D10).
  if (amp_links_) {
    auto worker = amp_worker_post;
    if (!worker) {
      worker = [](std::function<void()> task) { AppRuntime::PostWorkerNormal(std::move(task)); };
    }
    auto blob = std::make_unique<AmpChatBlobService>(*amp_links_, amp_io_pump, store_, identity_, worker);
    blob->Start();
    peer_blob_ = std::move(blob);

    auto history = std::make_unique<AmpChatHistoryService>(*amp_links_, amp_io_pump, store_, identity_, psk_store_,
                                                           worker);
    history->Start();
    peer_history_ = std::move(history);

    auto chat = std::make_unique<AmpDirectChatService>(*amp_links_, amp_io_pump, worker);
    chat->SetInboundHandler([this](RelayEnvelope envelope) { HandleDirectInbound(std::move(envelope)); });
    chat->Start();
    direct_chat_ = std::move(chat);
    log().info << "direct chat/history/blob transport=amp";
  } else {
    log().warning << "direct chat/history/blob unavailable (Amp required)";
  }
  chat_sync_ = std::make_unique<ChatSyncService>(store_, identity_, contacts_, relay_, *receive_pipeline_, inbox_,
                                                 peer_history_.get());
  chat_sync_->SetOnMessagesChanged([this]() {
    if (on_messages_changed_) {
      AppRuntime::PostUI([this]() { on_messages_changed_(); });
    }
  });
}

void MeshMessagingService::RegisterPeerSigningKey(const std::string& peer_identity_kind,
                                               const std::string& peer_identity_value,
                                               const std::string& signing_public_key_b64, const std::string& source) {
  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = signing_public_key_b64;
  record.source = source;
  signing_key_store_.Put(peer_identity_kind, peer_identity_value, std::move(record));
}

void MeshMessagingService::RegisterPeerKemKey(const std::string& peer_identity_kind,
                                           const std::string& peer_identity_value,
                                           const std::string& kem_public_key_b64, const std::string& source) {
  PeerKemKeyRecord record;
  record.kem_public_key_b64 = kem_public_key_b64;
  record.source = source;
  kem_key_store_.Put(peer_identity_kind, peer_identity_value, std::move(record));
}

void MeshMessagingService::RegisterMockPeerKeyForReply(const std::string& peer_identity_value) {
  RegisterPeerSigningKey(ContactIdKindToString(ContactIdKind::Account), peer_identity_value,
                         kMockPeerSigningPublicKeyB64, "mock_relay");
}

void MeshMessagingService::SetRelayClient(IRelayClient* relay) {
  relay_ = relay;
  relay_cursor_.clear();
  poll_pending_ = false;
  poll_again_ = false;
  if (auto identity = identity_.Get()) {
    LoadPersistedRelayCursor(identity->relay_user_id);
  }
  if (chat_sync_) {
    chat_sync_ = std::make_unique<ChatSyncService>(store_, identity_, contacts_, relay_, *receive_pipeline_, inbox_,
                                                 peer_history_.get());
    chat_sync_->SetOnMessagesChanged([this]() {
      if (on_messages_changed_) {
        AppRuntime::PostUI([this]() { on_messages_changed_(); });
      }
    });
  }
  TailSyncActiveE2eThread();
}

void MeshMessagingService::SetInitiationBillingStore(InitiationBillingStore* store) {
  initiation_billing_ = store;
  if (receive_pipeline_) {
    receive_pipeline_->SetInitiationBillingStore(store);
  }
}

void MeshMessagingService::BindCallControlInbound(CallControlInboundPorts ports) {
  call_control_ = std::move(ports);
  if (receive_pipeline_) {
    receive_pipeline_->BindCallControlInbound(call_control_);
  }
}

void MeshMessagingService::SetGroupMembership(GroupMembershipService* groups) {
  groups_ = groups;
}

void MeshMessagingService::SetProfileDataDir(std::string profile_data_dir) {
  profile_data_dir_ = std::move(profile_data_dir);
  if (peer_blob_) {
    peer_blob_->SetProfileDataDir(profile_data_dir_);
  }
}

IChatBlobPeerClient* MeshMessagingService::PeerBlobClient() const {
  return peer_blob_.get();
}

IChatBlobPeerService* MeshMessagingService::PeerBlobService() const {
  return peer_blob_.get();
}

void MeshMessagingService::LoadPersistedRelayCursor(const std::string& relay_user_id) {
  if (profile_data_dir_.empty()) {
    return;
  }
  const std::string loaded = LoadRelayInboxCursor(profile_data_dir_, relay_user_id);
  if (!loaded.empty()) {
    relay_cursor_ = loaded;
  }
}

void MeshMessagingService::PersistRelayCursor(const std::string& relay_user_id) {
  if (profile_data_dir_.empty() || relay_cursor_.empty()) {
    return;
  }
  SaveRelayInboxCursor(profile_data_dir_, relay_user_id, relay_cursor_);
}

void MeshMessagingService::RegisterPeerDirectEndpoint(const std::string& peer_relay_user_id,
                                                     const std::string& multiaddr) {
  const bool is_adp = IsAdpMultiaddr(multiaddr);
  if (amp_links_ && is_adp) {
    (void)amp_links_->RegisterEndpoint(peer_relay_user_id, multiaddr);
  } else if (amp_links_) {
    if (auto* amp_history = dynamic_cast<AmpChatHistoryService*>(peer_history_.get())) {
      amp_history->RegisterPeerEndpoint(peer_relay_user_id, multiaddr);
    }
  }
}

void MeshMessagingService::RegisterContactDirectEndpoints(const Contact& contact) {
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    return;
  }
  const auto register_ma = [this](const std::string& dial_key, const std::string& multiaddr) {
    if (!dial_key.empty() && !multiaddr.empty()) {
      RegisterPeerDirectEndpoint(dial_key, multiaddr);
    }
  };
  if (!contact.remote.endpoints.empty()) {
    for (const DirectoryEndpoint& endpoint : contact.remote.endpoints) {
      for (const std::string& ma : endpoint.multiaddrs) {
        register_ma(endpoint.peer_id, ma);
        register_ma(target.peer_identity_value, ma);
      }
    }
  } else {
    for (const std::string& ma : contact.multiaddrs) {
      register_ma(target.peer_identity_value, ma);
    }
  }
  for (const std::string& peer_id : PeerIdsFromContact(contact)) {
    if (amp_links_) {
      if (auto ma = amp_links_->PreferredMultiaddr(peer_id)) {
        register_ma(peer_id, *ma);
        if (target.peer_identity_value != peer_id) {
          register_ma(target.peer_identity_value, *ma);
        }
      }
    }
  }
}

void MeshMessagingService::SetPeerRouteSources(DirectoryShadowCache* shadows, IDirectoryClient* directory) {
  directory_shadows_ = shadows;
  directory_ = directory;
}

void MeshMessagingService::NoteAccountRelayRoute(const std::string& account_id,
                                                const std::string& relay_user_id) {
  if (!IsAccountIdentityValue(account_id) || !IsRelayUserIdValue(relay_user_id)) {
    return;
  }
  {
    std::lock_guard lock(account_relay_mutex_);
    account_to_relay_[account_id] = relay_user_id;
  }
  if (directory_shadows_) {
    DirectoryHit hit;
    hit.hit_id = relay_user_id;
    hit.account_id = account_id;
    hit.ids = {{ContactIdKind::Account, account_id, true},
               {ContactIdKind::RelayUser, relay_user_id, false}};
    directory_shadows_->Put(hit);
  }
}

void MeshMessagingService::RememberRouteFromEnvelope(const RelayEnvelope& envelope) {
  NoteAccountRelayRoute(envelope.sender_contact_id, envelope.sender_relay_id);
}

namespace {

void MaybePublishMemberJoinedAfterIngest(GroupMembershipService* groups, const RelayReceiveOutcome& outcome) {
  if (!outcome.publish_member_joined_group_id || !outcome.publish_member_joined_member_identity) {
    return;
  }
  if (!groups) {
    return;
  }
  (void)groups->PublishMemberJoined(*outcome.publish_member_joined_group_id,
                                    *outcome.publish_member_joined_member_identity,
                                    outcome.publish_member_joined_epoch);
}

} // namespace

void MeshMessagingService::MaybeSurfaceReceiveFailure(const RelayReceiveOutcome& outcome) {
  if (!outcome.receive_failure_notice || outcome.receive_failure_notice->empty()) {
    return;
  }
  const std::string& notice = *outcome.receive_failure_notice;
  log().warning << "Inbound receive failure"
                << " sender=" << (outcome.receive_failure_sender ? *outcome.receive_failure_sender : "")
                << " detail=" << outcome.receive_failure_detail << " notice=" << notice;

  // One toast/system line per sender+notice within the cooldown — old relay backlog must not flood UI.
  const std::string rate_key =
      (outcome.receive_failure_sender ? *outcome.receive_failure_sender : std::string{}) + "|" + notice;
  const int64_t now_ms = util::NowUnixMs();
  {
    std::lock_guard lock(receive_failure_mutex_);
    const auto it = receive_failure_last_ms_.find(rate_key);
    if (it != receive_failure_last_ms_.end() && now_ms - it->second < kReceiveFailureNoticeCooldownMs) {
      return;
    }
    receive_failure_last_ms_[rate_key] = now_ms;
    if (receive_failure_last_ms_.size() > 64) {
      // Drop stale entries so the map cannot grow without bound.
      for (auto iter = receive_failure_last_ms_.begin(); iter != receive_failure_last_ms_.end();) {
        if (now_ms - iter->second >= kReceiveFailureNoticeCooldownMs) {
          iter = receive_failure_last_ms_.erase(iter);
        } else {
          ++iter;
        }
      }
    }
  }

  if (outcome.receive_failure_thread_id && !outcome.receive_failure_thread_id->empty()) {
    ThreadMessage local;
    local.id = util::GenerateUuid();
    local.thread_id = *outcome.receive_failure_thread_id;
    local.sender_contact_id = kLocalSelfContactId;
    local.content_type = ChatContentType::System;
    local.text = notice;
    Object detail;
    detail.set("sender", outcome.receive_failure_sender.value_or(""));
    detail.set("detail", outcome.receive_failure_detail);
    Object payload;
    payload.set("control_type", "receive_failure");
    payload.set("detail", DumpJson(detail));
    local.payload_json = DumpJson(payload);
    local.timestamp = now_ms;
    local.delivery = MessageDelivery::Local;
    local.relay_visible = false;
    local.generation = "local";
    if (store_.AppendMessage(local)) {
      inbox_.OnInboundMessagePersisted(*outcome.receive_failure_thread_id, notice);
    }
  }

  if (on_delivery_notice_) {
    AppRuntime::PostUI([this, notice]() {
      if (on_delivery_notice_) {
        on_delivery_notice_(notice);
      }
    });
  }
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
}

void MeshMessagingService::HandleDirectInbound(RelayEnvelope envelope) {
  RememberRouteFromEnvelope(envelope);
  auto identity = identity_.Get();
  if (!identity) {
    return;
  }
  const RelayReceiveOutcome outcome =
      receive_pipeline_->ProcessEnvelope(envelope, identity->account_id, false, MessageTransport::Direct);
  MaybePublishMemberJoinedAfterIngest(groups_, outcome);
  MaybeSurfaceReceiveFailure(outcome);
  if (outcome.decision == IngestDecision::AcceptGap) {
    auto thread = store_.FindDirectThread(InboundTargetFromEnvelope(envelope));
    if (thread && *thread) {
      MaybeRepairGap((*thread)->id, envelope);
    }
  }
  if (outcome.persisted && !outcome.thread_id.empty()) {
    std::optional<std::string> preview;
    if (auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64)) {
      preview = decoded->text;
    }
    inbox_.OnInboundMessagePersisted(outcome.thread_id, preview);
    MaybeEnqueueAttachmentDownload(envelope, outcome.thread_id);
  }
  if (outcome.persisted || outcome.thread_changed) {
    if (on_messages_changed_) {
      AppRuntime::PostUI([this]() { on_messages_changed_(); });
    }
  }
}

void MeshMessagingService::TickMesh() {
  // Amp mesh pump is MeshHost::Tick via ConversationsHub; no PeerSessionManager.
}

void MeshMessagingService::WarmPeerForThread(const std::string& thread_id) {
  if (!amp_links_) {
    return;
  }
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Direct) {
    return;
  }
  const std::string peer = (*thread)->peer_identity_value;
  if (peer.empty()) {
    return;
  }
  {
    std::lock_guard lock(link_ux_mutex_);
    if (relay_fallback_notice_thread_id_ != thread_id) {
      relay_fallback_notice_thread_id_.clear();
      relay_fallback_notice_text_.clear();
    }
  }
  amp_links_->MarkWarm(peer);
  if (amp_links_->GetLinkSnapshot(peer).has_endpoint) {
    amp_links_->EnsureAssociation(peer, [](IChatPeerLinks::LinkRoe) {});
  }
}

ThreadPeerLinkView MeshMessagingService::GetThreadPeerLink(const std::string& thread_id) const {
  ThreadPeerLinkView view;
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Direct) {
    return view;
  }

  const std::string peer = (*thread)->peer_identity_value;
  view.relay_available = false;
  if (relay_ != nullptr &&
      ((*thread)->peer_identity_kind == ContactIdKindToString(ContactIdKind::Account) ||
       (*thread)->peer_identity_kind == "account")) {
    if (!(*thread)->participant_contact_ids.empty()) {
      if (auto contact = contacts_.Get((*thread)->participant_contact_ids.front())) {
        if (*contact) {
          for (const ContactId& id : (*contact)->ids) {
            if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
              view.relay_available = true;
              break;
            }
          }
        }
      }
    }
    if (!view.relay_available) {
      auto by_account = contacts_.FindByIdentity(peer, ContactIdKind::Account);
      if (by_account && by_account->has_value()) {
        for (const ContactId& id : (**by_account).ids) {
          if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
            view.relay_available = true;
            break;
          }
        }
      }
    }
  }

  if (peer.empty()) {
    view.phase = MeshPeerLinkPhase::Unavailable;
    view.status_label = "Can't connect";
    view.banner_message = "Add a Peer ID with multiaddr, or a Relay ID, to message.";
    view.show_banner = true;
    return view;
  }

  if (amp_links_) {
    const MeshPeerLinkSnapshot snap = amp_links_->GetLinkSnapshot(peer);
    view.phase = snap.phase;
    view.has_direct_endpoint = snap.has_endpoint;
    view.backoff_seconds = static_cast<int>((snap.backoff_remaining.count() + 999) / 1000);
    switch (snap.phase) {
    case MeshPeerLinkPhase::Connected:
      view.status_label = "Direct";
      break;
    case MeshPeerLinkPhase::Dialing:
    case MeshPeerLinkPhase::Handshaking:
      view.status_label = "Connecting…";
      view.banner_message = "Trying a direct link…";
      view.show_banner = true;
      break;
    case MeshPeerLinkPhase::Backoff:
      view.status_label = view.backoff_seconds > 0
                              ? ("Retrying soon (" + std::to_string(view.backoff_seconds) + "s)")
                              : "Retrying soon";
      view.banner_message = snap.detail.empty()
                                ? "Peer didn't answer — they may be offline or the address may be wrong."
                                : snap.detail;
      if (view.relay_available) {
        view.banner_message += " Messages can still go via relay.";
      }
      view.show_banner = true;
      view.show_retry = true;
      break;
    case MeshPeerLinkPhase::Idle:
      view.status_label = view.relay_available ? "Ready · relay available" : "Ready to connect";
      break;
    case MeshPeerLinkPhase::Unavailable:
    default:
      if (view.relay_available) {
        view.status_label = "Via relay";
      } else if (snap.has_endpoint) {
        view.status_label = "Offline";
        view.banner_message = "No usable peer address — add a dialable multiaddr on the contact.";
        view.show_banner = true;
      } else {
        view.status_label = "Can't connect";
        view.banner_message = "No usable peer address — add a dialable multiaddr on the contact.";
        view.show_banner = true;
      }
      break;
    }
    return view;
  }

  view.phase = MeshPeerLinkPhase::Unavailable;
  if (view.relay_available) {
    view.status_label = "Via relay";
  } else {
    view.status_label = "Offline";
    view.banner_message = "No usable peer address — add a dialable multiaddr on the contact.";
    view.show_banner = true;
  }
  return view;
}

void MeshMessagingService::RetryPeerDial(const std::string& thread_id) {
  if (!amp_links_) {
    return;
  }
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Direct) {
    return;
  }
  const std::string peer = (*thread)->peer_identity_value;
  if (peer.empty()) {
    return;
  }
  {
    std::lock_guard lock(link_ux_mutex_);
    if (relay_fallback_notice_thread_id_ == thread_id) {
      relay_fallback_notice_thread_id_.clear();
      relay_fallback_notice_text_.clear();
    }
  }
  amp_links_->MarkWarm(peer);
  if (amp_links_->GetLinkSnapshot(peer).has_endpoint) {
    amp_links_->EnsureAssociation(peer, [](IChatPeerLinks::LinkRoe) {});
  }
}

bool MeshMessagingService::IsE2ePrivateThread(const std::string& thread_id) const {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return false;
  }
  return (*thread)->kind == ThreadKind::Direct && (*thread)->channel == ThreadChannel::E2e;
}

bool MeshMessagingService::IsThreadCompromised(const std::string& thread_id) const {
  return IsE2ePrivateThread(thread_id) && IsE2eThreadCompromised(store_, thread_id);
}

bool MeshMessagingService::IsPskReadyToSend(const std::string& thread_id) const {
  if (!IsE2ePrivateThread(thread_id)) {
    return true;
  }
  auto status = psk_coordinator_.GetStatus(thread_id);
  return status && status->has_psk && status->verified;
}

bool MeshMessagingService::HasActiveLocalCall() const {
  if (!call_control_.has_active_local_call) {
    return false;
  }
  return call_control_.has_active_local_call();
}

Roe<void> MeshMessagingService::MaybeSendPublicAutoRekey(const std::string& thread_id) {
  if (HasActiveLocalCall()) {
    return {};
  }
  auto should = public_lock_.ShouldAutoRekey(thread_id, util::NowUnixMs());
  if (!should) {
    return should.error();
  }
  if (!*should) {
    return {};
  }
  auto sent = SendPublicPskRotate(thread_id, PublicPskRotateKind::Auto);
  if (!sent) {
    log().warning << "Public auto-rekey skipped: " << sent.error().message;
  }
  return {};
}

Roe<ThreadMessage> MeshMessagingService::SendPublicPskRotate(const std::string& thread_id,
                                                           const PublicPskRotateKind kind) {
  if (HasActiveLocalCall()) {
    return Error("Finish the call first");
  }
  ByteVector peer_account_kem_pk;
  if (kind == PublicPskRotateKind::Lock) {
    auto thread = store_.GetThread(thread_id);
    if (!thread) {
      return thread.error();
    }
    if (!*thread) {
      return Error("Thread not found");
    }
    const ChatTargetKey key = E2eRelayPayloadCodec::ChatTargetFromThread(**thread);
    auto peer_kem = kem_key_resolver_.Resolve(key.peer_identity_kind, key.peer_identity_value);
    if (!peer_kem) {
      return peer_kem.error();
    }
    auto decoded = Base64Decode(peer_kem->kem_public_key_b64);
    if (!decoded) {
      return decoded.error();
    }
    peer_account_kem_pk = std::move(*decoded);
  }
  auto plan = kind == PublicPskRotateKind::Lock
                  ? public_lock_.PrepareLock(thread_id, peer_account_kem_pk, util::NowUnixMs())
                  : public_lock_.PrepareAutoRekey(thread_id, util::NowUnixMs());
  if (!plan) {
    return plan.error();
  }
  auto payload = PskRotateCodec::EncodePayloadJson(plan->detail);
  if (!payload) {
    (void)public_lock_.AbortPrepare(*plan);
    return payload.error();
  }
  SendRelayOptions opts;
  opts.content_type = ChatContentType::System;
  opts.payload_json = *payload;
  opts.key_init_b64 = plan->key_init_b64;
  const char* text =
      kind == PublicPskRotateKind::Lock ? "New messages stay on this device." : "Chat key updated.";
  auto sent = SendUserMessage(thread_id, text, opts);
  if (!sent) {
    (void)public_lock_.AbortPrepare(*plan);
    return sent.error();
  }
  if (auto committed = public_lock_.Commit(*plan, util::NowUnixMs()); !committed) {
    return committed.error();
  }
  PurgeRetryQueueForThread(thread_id);
  return sent;
}

Roe<void> MeshMessagingService::LockPublicThreadToThisDevice(const std::string& thread_id) {
  auto sent = SendPublicPskRotate(thread_id, PublicPskRotateKind::Lock);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<PublicKeyScope> MeshMessagingService::GetPublicKeyScope(const std::string& thread_id) const {
  return public_lock_.GetKeyScope(thread_id);
}

Roe<bool> MeshMessagingService::CanLockPublicToThisDevice(const std::string& thread_id) const {
  if (!support_account_id_.empty()) {
    auto thread = store_.GetThread(thread_id);
    if (thread && *thread && (*thread)->peer_identity_value == support_account_id_) {
      return false;
    }
  }
  return public_lock_.CanLockToThisDevice(thread_id);
}

void MeshMessagingService::SetSupportAccountId(std::string account_id) {
  support_account_id_ = std::move(account_id);
}

void MeshMessagingService::PurgeRetryQueueForThread(const std::string& thread_id) {
  std::lock_guard lock(retry_mutex_);
  retry_queue_.erase(std::remove_if(retry_queue_.begin(), retry_queue_.end(),
                                     [&](const PendingRelaySend& item) { return item.thread_id == thread_id; }),
                     retry_queue_.end());
}

Roe<uint32_t> MeshMessagingService::StartNewSecureChat(const std::string& thread_id) {
  if (!IsE2ePrivateThread(thread_id)) {
    return Error("Not a private E2E thread");
  }
  auto new_epoch = epoch_coordinator_.StartNewSecureChat(thread_id);
  if (!new_epoch) {
    return new_epoch.error();
  }
  PurgeRetryQueueForThread(thread_id);
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
  return *new_epoch;
}

Roe<void> MeshMessagingService::PauseIntegrityOnly(const std::string& thread_id) {
  if (!IsE2ePrivateThread(thread_id)) {
    return Error("Not a private E2E thread");
  }
  PurgeRetryQueueForThread(thread_id);
  return epoch_coordinator_.PauseOnly(thread_id);
}

Roe<std::string> MeshMessagingService::RotatePskAndExportBundle(const std::string& thread_id) {
  if (!IsE2ePrivateThread(thread_id)) {
    return Error("Not a private E2E thread");
  }
  auto bundle = psk_coordinator_.RotatePskAndExportBundle(thread_id, util::NowUnixMs());
  if (!bundle) {
    return bundle.error();
  }
  PurgeRetryQueueForThread(thread_id);
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
  return bundle;
}

Roe<PskSessionStatus> MeshMessagingService::GetPskStatus(const std::string& thread_id) const {
  return psk_coordinator_.GetStatus(thread_id);
}

Roe<PskExportView> MeshMessagingService::EnsurePskGenerated(const std::string& thread_id) {
  return psk_coordinator_.EnsureGenerated(thread_id);
}

Roe<PskExportView> MeshMessagingService::GetPskExportView(const std::string& thread_id) const {
  return psk_coordinator_.GetExportView(thread_id);
}

Roe<std::string> MeshMessagingService::ExportPskBundleJson(const std::string& thread_id) const {
  return psk_coordinator_.ExportBundleJson(thread_id);
}

Roe<void> MeshMessagingService::ImportPskRawBase64(const std::string& thread_id, const std::string& raw_b64) {
  return psk_coordinator_.ImportRawBase64(thread_id, raw_b64);
}

Roe<void> MeshMessagingService::ImportPskBundleJson(const std::string& thread_id, const std::string& bundle_json) {
  return psk_coordinator_.ImportBundleJson(thread_id, bundle_json);
}

Roe<void> MeshMessagingService::MarkPskVerified(const std::string& thread_id) {
  return psk_coordinator_.MarkVerified(thread_id, util::NowUnixMs());
}

void MeshMessagingService::ScrollBackfill(const std::string& thread_id,
                                         std::function<void(Roe<ChatSyncResult>)> on_complete) {
  RunSyncOnIo(thread_id, [this, thread_id]() { return chat_sync_->ScrollBackfill(thread_id); },
              std::move(on_complete));
}

void MeshMessagingService::TailSyncActiveE2eThread() {
  const std::string& active_id = inbox_.ActiveThreadId();
  if (active_id.empty() || !IsE2ePrivateThread(active_id)) {
    return;
  }
  MaybeTailSync(active_id);
}

void MeshMessagingService::RunSyncOnIo(const std::string& thread_id,
                                      std::function<Roe<ChatSyncResult>()> task,
                                      std::function<void(Roe<ChatSyncResult>)> on_complete) {
  if (!chat_sync_ || !IsE2ePrivateThread(thread_id)) {
    if (on_complete) {
      AppRuntime::PostUI(
                              [on_complete = std::move(on_complete)]() { on_complete(Error("Sync not available")); });
    }
    return;
  }

  bool expected = false;
  if (!sync_pending_.compare_exchange_strong(expected, true)) {
    if (on_complete) {
      AppRuntime::PostUI([on_complete = std::move(on_complete)]() {
        on_complete(Error("Sync already in progress"));
      });
    }
    return;
  }

  AppRuntime::PostWorkerNormal([this, thread_id, task = std::move(task),
                                                  on_complete = std::move(on_complete)]() mutable {
    struct SyncGuard {
      std::atomic<bool>& pending;
      ~SyncGuard() { pending = false; }
    } guard{sync_pending_};

    Roe<ChatSyncResult> result = task();
    if (result && on_messages_changed_) {
      AppRuntime::PostUI([this]() { on_messages_changed_(); });
    }
    if (on_complete) {
      AppRuntime::PostUI(
                              [on_complete = std::move(on_complete), result = std::move(result)]() mutable {
                                on_complete(std::move(result));
                              });
    }
  });
}

void MeshMessagingService::SyncWithPeer(const std::string& thread_id,
                                       std::function<void(Roe<ChatSyncResult>)> on_complete) {
  RunSyncOnIo(thread_id, [this, thread_id]() { return chat_sync_->UserInitiatedSync(thread_id); },
              std::move(on_complete));
}

void MeshMessagingService::RetryGapSync(const std::string& thread_id,
                                       std::function<void(Roe<ChatSyncResult>)> on_complete) {
  RunSyncOnIo(thread_id, [this, thread_id]() { return chat_sync_->RetryGapSync(thread_id); }, std::move(on_complete));
}

std::optional<std::string> MeshMessagingService::ResolvePeerRelayId(const Thread& thread) const {
  std::unordered_map<std::string, std::string> learned;
  {
    std::lock_guard lock(account_relay_mutex_);
    learned = account_to_relay_;
  }
  if (auto from_local = ResolvePeerBriefRoute(thread, contacts_, learned)) {
    return from_local;
  }
  if (directory_shadows_ && IsAccountIdentityValue(thread.peer_identity_value)) {
    if (auto hit = directory_shadows_->Get(thread.peer_identity_value)) {
      if (auto relay = RelayUserIdFromDirectoryHit(*hit)) {
        return relay;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> MeshMessagingService::ResolvePeerRelayIdWithDirectory(const Thread& thread) {
  if (auto resolved = ResolvePeerRelayId(thread)) {
    return resolved;
  }
  if (!directory_ || !IsAccountIdentityValue(thread.peer_identity_value)) {
    return std::nullopt;
  }
  auto hit = directory_->LookupByAccount(thread.peer_identity_value);
  if (!hit) {
    log().warning << "LookupByAccount for Brief route failed peer=" << thread.peer_identity_value
                  << " err=" << hit.error().message;
    return std::nullopt;
  }
  auto relay = RelayUserIdFromDirectoryHit(*hit);
  if (!relay) {
    return std::nullopt;
  }
  NoteAccountRelayRoute(thread.peer_identity_value, *relay);
  return relay;
}

TrustLevel MeshMessagingService::ResolveThreadTrust(const Thread& thread) const {
  if (thread.participant_contact_ids.empty()) {
    return TrustLevel::Unknown;
  }
  const auto contact = contacts_.Get(thread.participant_contact_ids.front());
  if (!contact || !*contact) {
    return TrustLevel::Unknown;
  }
  return (*contact)->trust;
}

void MeshMessagingService::EnqueueRetry(PendingRelaySend pending) {
  std::lock_guard lock(retry_mutex_);
  retry_queue_.push_back(std::move(pending));
}

void MeshMessagingService::NotifyDeliveryIssue(const Thread& thread, const std::string& error_message) {
  if (!on_delivery_notice_) {
    return;
  }
  const bool rate_limited = error_message.find("rate limit") != std::string::npos;
  std::string notice;
  if (error_message.empty()) {
    notice = error_message;
  } else if (error_message.find("host not running") != std::string::npos) {
    notice = "Direct messaging is off — check Me → Network.";
  } else if (error_message.find("Empty peer endpoint") != std::string::npos ||
             error_message.find("Invalid multiaddr") != std::string::npos ||
             error_message.find("missing /p2p/") != std::string::npos ||
             error_message.find("Invalid PeerId") != std::string::npos) {
    notice = "Peer address looks wrong — edit the contact multiaddr.";
  } else if (error_message.find("not registered") != std::string::npos ||
             error_message.find("missing PeerInfo") != std::string::npos ||
             error_message.find("Peer-direct endpoint") != std::string::npos) {
    notice = "No usable peer address — add a dialable multiaddr on the contact.";
  } else if (error_message.find("backoff") != std::string::npos) {
    notice = "Waiting before retrying the peer connection.";
  } else if (error_message.find("Too many concurrent") != std::string::npos) {
    notice = "Busy connecting to other peers — try again shortly.";
  } else if (error_message.find("dial failed") != std::string::npos ||
             error_message.find("timed out") != std::string::npos ||
             error_message.find("timeout") != std::string::npos) {
    notice = "Peer didn't answer — they may be offline or the address may be wrong.";
  } else if (error_message.find("stream open failed") != std::string::npos) {
    notice = "Reached the peer but chat handshake failed.";
  } else if (error_message.find("Failed to send") != std::string::npos ||
             error_message.find("Failed to read") != std::string::npos ||
             error_message.find("ack") != std::string::npos) {
    notice = "Direct send didn't confirm — will use relay if available.";
  } else if (error_message.find("Relay client not configured") != std::string::npos) {
    notice = "Couldn't deliver — relay isn't configured.";
  } else {
    notice = error_message;
  }
  if (rate_limited) {
    if (ResolveThreadTrust(thread) == TrustLevel::Friendly) {
      notice = "Relay is busy — your message will retry shortly.";
    } else {
      notice = "Relay rate limit — slow down messaging with new contacts.";
    }
  }
  AppRuntime::PostUI([this, notice]() {
    if (on_delivery_notice_) {
      on_delivery_notice_(notice);
    }
  });
}

void MeshMessagingService::NotifyRelayFallback(const std::string& thread_id) {
  const std::string notice = "Couldn't reach peer directly — sent via relay.";
  {
    std::lock_guard lock(link_ux_mutex_);
    relay_fallback_notice_thread_id_ = thread_id;
    relay_fallback_notice_text_ = notice;
  }
  if (!on_delivery_notice_) {
    return;
  }
  AppRuntime::PostUI([this, notice]() {
    if (on_delivery_notice_) {
      on_delivery_notice_(notice);
    }
  });
}

void MeshMessagingService::MaybeTailSync(const std::string& thread_id) {
  WarmPeerForThread(thread_id);
  if (!chat_sync_) {
    return;
  }
  AppRuntime::PostWorkerNormal([this, thread_id]() { (void)chat_sync_->TailSync(thread_id); });
}

void MeshMessagingService::MaybeRepairGap(const std::string& thread_id, const RelayEnvelope& envelope) {
  if (!chat_sync_ || !envelope.session_epoch) {
    return;
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, envelope.session_epoch);
  if (!sync_state || sync_state->phase == PeerSyncPhase::Compromised) {
    return;
  }
  if (envelope.sender_seq <= sync_state->contiguous_peer_seq + 1) {
    return;
  }
  const uint64_t gap_min = sync_state->contiguous_peer_seq + 1;
  const uint64_t gap_max = envelope.sender_seq - 1;
  (void)chat_sync_->RepairGap(thread_id, gap_min, gap_max);
}

void MeshMessagingService::ApplySendResult(const std::string& thread_id, const std::string& message_id, bool success,
                                          const std::string& error_message, MessageTransport transport,
                                          bool relay_after_direct_attempt) {
  auto messages = store_.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!messages) {
    return;
  }
  for (ThreadMessage& existing : *messages) {
    if (existing.id == message_id) {
      existing.delivery = success ? MessageDelivery::Relayed : MessageDelivery::Failed;
      if (success) {
        existing.transport = transport;
      }
      (void)store_.UpdateMessage(existing);
      break;
    }
  }
  if (success && transport == MessageTransport::Relay && relay_after_direct_attempt) {
    NotifyRelayFallback(thread_id);
  }
  if (!success) {
    auto thread = store_.GetThread(thread_id);
    if (thread && *thread) {
      NotifyDeliveryIssue(**thread, error_message);
    }
  }
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
}

Roe<ThreadMessage> MeshMessagingService::SendUserMessage(const std::string& thread_id, const std::string& text,
                                                         const SendRelayOptions& options) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread || (*thread)->kind != ThreadKind::Direct) {
    return Error("Not a direct thread");
  }
  if ((*thread)->channel == ThreadChannel::E2e && IsThreadCompromised(thread_id)) {
    return Error("Messaging paused — rotate the encryption key or pause only");
  }
  if ((*thread)->channel == ThreadChannel::E2e && !IsPskReadyToSend(thread_id)) {
    return Error("Verify the encryption key with your contact before sending");
  }
  if ((*thread)->channel == ThreadChannel::E2ePublic) {
    auto scope = public_lock_.GetKeyScope(thread_id);
    if (!scope) {
      return scope.error();
    }
    if (*scope == PublicKeyScope::LockedOut) {
      return Error("This chat continues on another device");
    }
  }
  if ((*thread)->peer_identity_value.empty()) {
    return Error("Direct thread missing peer identity");
  }
  // P001: first initiate blocked when peer floor > 0 and payment rails unavailable.
  // System/control traffic (call invite, charge_required, …) must not hit this gate.
  const bool system_control =
      options.content_type && *options.content_type == ChatContentType::System;
  if (!system_control && (*thread)->channel == ThreadChannel::E2ePublic) {
    if (auto rotated = MaybeSendPublicAutoRekey(thread_id); !rotated) {
      return rotated.error();
    }
  }
  if (!system_control && initiation_billing_ &&
      !initiation_billing_->IsOpen((*thread)->peer_identity_value)) {
    const InitiationPeerBilling billing = initiation_billing_->Get((*thread)->peer_identity_value);
    const int64_t offer = InitiationPricing::DefaultOfferForFloor(billing.floor_minor);
    if (auto payable = InitiationPricing::CheckOutboundPayable(offer); !payable) {
      return payable.error();
    }
    if (auto floor_ok = InitiationPricing::CheckOfferAgainstFloor(offer, billing.floor_minor); !floor_ok) {
      return floor_ok.error();
    }
    if (offer > 0) {
      (void)initiation_billing_->MarkOffered((*thread)->peer_identity_value, offer, billing.floor_minor);
    } else {
      (void)initiation_billing_->MarkOpen((*thread)->peer_identity_value);
    }
  }

  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = thread_id;
  message.sender_contact_id = options.sender_contact_id.value_or(kLocalSelfContactId);
  message.text = text;
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Pending;
  message.relay_visible = true;
  message.transport = MessageTransport::Relay;
  message.generation = options.generation;
  message.ai_invoke_mode = options.ai_invoke_mode;
  message.seq_owner_contact_id = options.seq_owner_contact_id;
  ApplyRichPayloadOptions(message, options);

  auto sender_seq = store_.AllocateSenderSeq(thread_id);
  if (!sender_seq) {
    return sender_seq.error();
  }
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    return session_epoch.error();
  }
  message.sender_seq = *sender_seq;
  message.session_epoch = *session_epoch;

  if (auto cap = EnsureAnnotationCap(store_, thread_id, message); !cap) {
    return cap.error();
  }

  auto appended = store_.AppendMessage(message);
  if (!appended) {
    return appended.error();
  }

  if (options.update_preview) {
    (void)inbox_.UpdatePreview(thread_id, text);
  }

  auto identity = identity_.Get();
  if (!identity) {
    appended->delivery = MessageDelivery::Failed;
    (void)store_.UpdateMessage(*appended);
    return appended.error();
  }

  auto peer_relay_id = ResolvePeerRelayIdWithDirectory(**thread);
  const std::string amp_peer_key = (*thread)->peer_identity_value;
  const bool amp_reachable =
      direct_chat_ && !amp_peer_key.empty() && direct_chat_->IsPeerReachable(amp_peer_key);
  if (!peer_relay_id && !amp_reachable) {
    appended->delivery = MessageDelivery::Failed;
    (void)store_.UpdateMessage(*appended);
    return Error("Direct thread missing peer relay id");
  }

  std::optional<std::string> payload_b64;
  std::optional<std::string> key_init_b64;
  if (E2eRelayPayloadCodec::RequiresEncryption((*thread)->channel)) {
    const ChatTargetKey target_key = E2eRelayPayloadCodec::ChatTargetFromThread(**thread);
    ByteVector master_psk;
    auto master_psk_b64 = psk_store_.ResolveMasterPskForEpoch(target_key, *message.session_epoch);
    if (!master_psk_b64) {
      appended->delivery = MessageDelivery::Failed;
      (void)store_.UpdateMessage(*appended);
      return master_psk_b64.error();
    }
    if (!master_psk_b64->has_value()) {
      if (options.key_init_b64 && !options.key_init_b64->empty()) {
        appended->delivery = MessageDelivery::Failed;
        (void)store_.UpdateMessage(*appended);
        return Error("psk_rotate requires the current encryption key");
      }
      if ((*thread)->channel != ThreadChannel::E2ePublic) {
        appended->delivery = MessageDelivery::Failed;
        (void)store_.UpdateMessage(*appended);
        return Error("PSK not configured for this chat");
      }
      auto peer_kem = kem_key_resolver_.Resolve(target_key.peer_identity_kind, target_key.peer_identity_value);
      if (!peer_kem) {
        appended->delivery = MessageDelivery::Failed;
        (void)store_.UpdateMessage(*appended);
        return peer_kem.error();
      }
      auto peer_public = Base64Decode(peer_kem->kem_public_key_b64);
      if (!peer_public) {
        appended->delivery = MessageDelivery::Failed;
        (void)store_.UpdateMessage(*appended);
        return peer_public.error();
      }
      // Directory KEM is the recipient account key (M015).
      auto established = AutoKeyEstablishment::EncapsulateForRecipient(*peer_public);
      if (!established) {
        appended->delivery = MessageDelivery::Failed;
        (void)store_.UpdateMessage(*appended);
        return established.error();
      }
      master_psk = std::move(established->master_psk);
      key_init_b64 = std::move(established->key_init_b64);
      PskSessionRecord record;
      record.key = target_key;
      record.session_epoch = *message.session_epoch;
      record.master_psk_b64 = Base64Encode(master_psk);
      if (auto saved = psk_store_.Save(record); !saved) {
        appended->delivery = MessageDelivery::Failed;
        (void)store_.UpdateMessage(*appended);
        return saved.error();
      }
    } else {
      auto decoded = Base64Decode(**master_psk_b64);
      if (!decoded) {
        appended->delivery = MessageDelivery::Failed;
        (void)store_.UpdateMessage(*appended);
        return decoded.error();
      }
      master_psk = std::move(*decoded);
      if (options.key_init_b64 && !options.key_init_b64->empty()) {
        key_init_b64 = options.key_init_b64;
      }
    }
    E2eEncryptParams params;
    params.text = text;
    params.channel = E2eRelayPayloadCodec::ChannelFromThread((*thread)->channel);
    params.peer_contact_id = (*thread)->peer_identity_value;
    params.sender_contact_id = identity->account_id;
    params.message_id = message.id;
    params.sender_seq = *message.sender_seq;
    params.session_epoch = *message.session_epoch;
    params.timestamp = message.timestamp;
    Roe<E2eEncryptResult> encrypted = [&]() -> Roe<E2eEncryptResult> {
      if (options.content_type && *options.content_type != ChatContentType::Text) {
        auto plaintext = ChatPayloadCodec::EncodeToRow(message);
        if (!plaintext) {
          return plaintext.error();
        }
        return E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(params, *plaintext, master_psk, key_init_b64);
      }
      return E2eRelayPayloadCodec::EncryptTextWithAutoKey(params, master_psk, key_init_b64);
    }();
    if (!encrypted) {
      appended->delivery = MessageDelivery::Failed;
      (void)store_.UpdateMessage(*appended);
      return encrypted.error();
    }
    payload_b64 = std::move(encrypted->payload_b64);
    key_init_b64 = std::move(encrypted->key_init_b64);
  } else {
    auto plaintext = RelayWirePayload::EncodePlaintextText(text);
    if (!plaintext) {
      appended->delivery = MessageDelivery::Failed;
      (void)store_.UpdateMessage(*appended);
      return plaintext.error();
    }
    payload_b64 = std::move(*plaintext);
  }

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = message.id;
  envelope.sender_relay_id = identity->relay_user_id;
  envelope.sender_contact_id = identity->account_id;
  envelope.route.kind = "direct";
  envelope.route.channel = (*thread)->channel;
  envelope.body.e2e.payload_b64 = *payload_b64;
  if (key_init_b64 && !key_init_b64->empty()) {
    envelope.body.e2e.key_init_b64 = std::move(*key_init_b64);
  }
  envelope.sender_seq = *message.sender_seq;
  envelope.order_key = envelope.sender_seq;
  envelope.session_epoch = *message.session_epoch;
  // Brief recipient is relay:; Amp-only (no route yet) uses Account as party placeholder.
  const std::string route_party = peer_relay_id ? *peer_relay_id : amp_peer_key;
  envelope.stream_key =
      BuildCanonicalRelayStreamKey(identity->relay_user_id, route_party, (*thread)->channel, *message.session_epoch);
  envelope.recipient_contact_id = route_party;
  envelope.timestamp = message.timestamp;

  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  if (!sign_bytes) {
    appended->delivery = MessageDelivery::Failed;
    (void)store_.UpdateMessage(*appended);
    return sign_bytes.error();
  }
  auto signature = identity_.SignBytes(*sign_bytes);
  if (signature) {
    envelope.signature = *signature;
  }

  if (auto* mock_relay = dynamic_cast<MockRelayClient*>(relay_)) {
    mock_relay->SetNextReplySenderId((*thread)->peer_identity_value);
    RegisterMockPeerKeyForReply((*thread)->peer_identity_value);
  }

  // Amp dial key is Account ID (invite listen multiaddrs / contact endpoints). Brief uses relay:.
  auto send_work = [this, thread_id, envelope, message_id = message.id, amp_peer_key,
                    peer_relay_id, critical_lane = options.critical_lane]() mutable {
    bool tried_direct = false;
    // Call-control (critical_lane): Amp chat Open+ack-per-message races SoftMigrate/media and
    // burns up to ~4s before Brief — noisy WARNINGs for 10–20s while signaling still works via
    // relay. Prefer Brief when a relay route is known; Amp remains for Amp-only peers.
    const bool try_amp = direct_chat_ && !amp_peer_key.empty() &&
                         direct_chat_->IsPeerReachable(amp_peer_key) &&
                         !(critical_lane && peer_relay_id.has_value());
    if (try_amp) {
      tried_direct = true;
      const auto direct = direct_chat_->SendEnvelope(amp_peer_key, envelope);
      if (direct) {
        ApplySendResult(thread_id, message_id, true, {}, MessageTransport::Direct);
        return;
      }
      if (critical_lane) {
        log().warning << "Amp call-control send failed message_id=" << message_id
                      << " peer=" << amp_peer_key << " err=" << direct.error().message
                      << " (will try Brief if relay route known)";
      }
    }
    if (!peer_relay_id) {
      ApplySendResult(thread_id, message_id, false,
                      tried_direct ? "mesh dial failed" : "Direct thread missing peer relay id");
      return;
    }
    if (!relay_) {
      ApplySendResult(thread_id, message_id, false,
                      tried_direct ? "mesh dial failed" : "Relay client not configured");
      return;
    }
    const auto result = relay_->Send(envelope);
    if (!result) {
      log().warning << "Relay Send failed message_id=" << message_id << " peer=" << *peer_relay_id
                    << " err=" << result.error().message;
      EnqueueRetry(PendingRelaySend{.envelope = envelope, .message_id = message_id, .thread_id = thread_id,
                                    .attempt_count = 1});
      ApplySendResult(thread_id, message_id, false, result.error().message);
      return;
    }
    if (critical_lane) {
      log().info << "Relay Send ok (call-control) message_id=" << message_id
                 << " peer=" << *peer_relay_id;
    }
    ApplySendResult(thread_id, message_id, true, {}, MessageTransport::Relay, tried_direct);
  };
  if (options.critical_lane) {
    // Call-control (MediaKey/Accept) must not sit behind PollInbox on Normal workers.
    AppRuntime::PostWorkerCritical(std::move(send_work));
  } else {
    AppRuntime::PostWorkerNormal( std::move(send_work));
  }

  // Always hop to UI — SendUserMessage runs on IO during membership fan-out (PublishMemberJoined).
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
  if (!system_control && (*thread)->channel == ThreadChannel::E2ePublic) {
    (void)public_lock_.NoteTraffic(thread_id);
  }
  return *appended;
}

Roe<void> MeshMessagingService::SendChargeRequired(const std::string& peer_identity,
                                                  const std::optional<int64_t> floor_minor) {
  if (peer_identity.empty()) {
    return Error("Missing peer identity");
  }
  if (!initiation_billing_) {
    return Error("Initiation billing unavailable");
  }
  auto identity = identity_.Get();
  if (!identity) {
    return Error("Local identity unavailable");
  }
  const int64_t floor = floor_minor.value_or(identity->initiation_floor);

  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = peer_identity;
  if (auto contact = contacts_.FindByIdentity(peer_identity, ContactIdKind::Account)) {
    if (*contact) {
      contact_id = (*contact)->id;
      dm_title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (dm_title.empty()) {
        dm_title = peer_identity;
      }
    }
  }

  auto thread = store_.FindOrCreateDirectThread(direct_target, contact_id, dm_title);
  if (!thread) {
    return thread.error();
  }

  ChargeRequiredDetail detail;
  detail.peer_identity = identity->account_id.empty() ? identity->relay_user_id : identity->account_id;
  detail.floor_minor = floor;
  detail.currency = kPricingCurrencyId;
  auto detail_json = InitiationBillingCodec::EncodeChargeRequired(detail);
  if (!detail_json) {
    return detail_json.error();
  }

  SendRelayOptions opts;
  opts.content_type = ChatContentType::System;
  Object payload;
  payload.set("control_type",
              InitiationBillingCodec::ControlTypeToWire(InitiationBillingControlType::ChargeRequired));
  payload.set("detail", *detail_json);
  opts.payload_json = DumpJson(payload);
  opts.generation = "system";
  opts.update_preview = false;
  opts.critical_lane = true;
  opts.sender_contact_id = identity->account_id;

  auto sent = SendUserMessage(thread->id, "Charge required", opts);
  if (!sent) {
    return sent.error();
  }
  // Re-lock after send succeeds (send path skips initiation gate for system controls).
  (void)initiation_billing_->SetFloor(peer_identity, floor);
  (void)initiation_billing_->MarkClosed(peer_identity);
  log().info << "charge_required sent peer=" << peer_identity << " floor=" << floor;
  return {};
}

Roe<ThreadMessage> MeshMessagingService::SendGroupMessage(const std::string& thread_id, const std::string& text,
                                                          const SendRelayOptions& options) {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Group || !(*thread)->group_id) {
    return Error("Not a group thread");
  }

  auto identity = identity_.Get();
  if (!identity || identity->relay_user_id.empty()) {
    return Error("Local relay identity unavailable");
  }

  auto members = group_roster_.ListMembers(*(*thread)->group_id);
  if (!members) {
    return members.error();
  }

  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = thread_id;
  message.sender_contact_id = options.sender_contact_id.value_or(kLocalSelfContactId);
  message.text = text;
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Pending;
  message.relay_visible = true;
  message.transport = MessageTransport::Relay;
  ApplyRichPayloadOptions(message, options);

  auto sender_seq = store_.AllocateSenderSeq(thread_id);
  if (!sender_seq) {
    return sender_seq.error();
  }
  auto session_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!session_epoch) {
    return session_epoch.error();
  }
  message.sender_seq = *sender_seq;
  message.session_epoch = *session_epoch;

  if (auto cap = EnsureAnnotationCap(store_, thread_id, message); !cap) {
    return cap.error();
  }

  auto appended = store_.AppendMessage(message);
  if (!appended) {
    return appended.error();
  }
  if (options.update_preview) {
    (void)inbox_.UpdatePreview(thread_id, text);
  }

  std::vector<GroupMemberTarget> targets;
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity == identity->account_id) {
      continue;
    }
    GroupMemberTarget target;
    target.member_identity = member.member_identity;
    target.target_key = GroupE2ePayloadCodec::PairTargetKey(member.member_identity);
    targets.push_back(std::move(target));
  }

  auto resolve_kem = [this](const ChatTargetKey& key) -> Roe<ByteVector> {
    auto kem = kem_key_resolver_.Resolve(key.peer_identity_kind, key.peer_identity_value);
    if (!kem) {
      return kem.error();
    }
    return Base64Decode(kem->kem_public_key_b64);
  };

  std::optional<std::vector<uint8_t>> rich_plaintext;
  if (options.content_type && *options.content_type != ChatContentType::Text) {
    auto plaintext = ChatPayloadCodec::EncodeToRow(message);
    if (!plaintext) {
      appended->delivery = MessageDelivery::Failed;
      (void)store_.UpdateMessage(*appended);
      return plaintext.error();
    }
    rich_plaintext = std::move(*plaintext);
  }

  const std::string group_id = *(*thread)->group_id;
  auto encrypted = GroupE2ePayloadCodec::EncryptForMembers(
      text, group_id, identity->account_id, message.id, *message.sender_seq, *message.session_epoch,
      message.timestamp, targets, psk_store_, resolve_kem, rich_plaintext);
  if (!encrypted) {
    appended->delivery = MessageDelivery::Failed;
    (void)store_.UpdateMessage(*appended);
    if (groups_) {
      for (const GroupMemberTarget& target : targets) {
        groups_->MarkMemberUnreachable(group_id, target.member_identity);
      }
    }
    return encrypted.error();
  }

  if (groups_) {
    for (const std::string& failed_id : encrypted->failed_member_identities) {
      groups_->MarkMemberUnreachable(group_id, failed_id);
    }
  }

  for (const auto& [recipient, payload_b64] : encrypted->member_payloads) {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = message.id;
    envelope.sender_relay_id = identity->relay_user_id;
    envelope.sender_contact_id = identity->account_id;
    envelope.route.kind = "group";
    envelope.route.group_id = group_id;
    envelope.body.e2e.member_payloads = std::map<std::string, std::string>{{recipient, payload_b64}};
    envelope.sender_seq = *message.sender_seq;
    envelope.order_key = envelope.sender_seq;
    envelope.session_epoch = *message.session_epoch;
    envelope.stream_key = BuildGroupRelayStreamKey(group_id, *message.session_epoch);
    envelope.recipient_contact_id = recipient;
    envelope.timestamp = message.timestamp;

    auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
    if (!sign_bytes) {
      if (groups_) {
        groups_->MarkMemberUnreachable(group_id, recipient);
      }
      continue;
    }
    if (auto signature = identity_.SignBytes(*sign_bytes)) {
      envelope.signature = *signature;
    }
    if (relay_) {
      (void)relay_->Send(envelope);
    }
    if (groups_) {
      groups_->ClearMemberUnreachable(group_id, recipient);
    }
  }

  if (!encrypted->failed_member_identities.empty() && on_delivery_notice_) {
    AppRuntime::PostUI([this]() {
      if (on_delivery_notice_) {
        on_delivery_notice_("Some group members couldn’t receive this message");
      }
    });
  }

  appended->delivery = MessageDelivery::Relayed;
  (void)store_.UpdateMessage(*appended);
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
  return *appended;
}

namespace {

Roe<ThreadMessage> SendAnnotationReaction(MeshMessagingService& mesh_messaging, IThreadStore& store, const std::string& thread_id,
                                          const std::string& target_message_id, const std::string& emoji,
                                          const char* annotation_type) {
  if (target_message_id.empty()) {
    return Error("Missing target message");
  }
  const std::string trimmed = NormalizeEmojiKey(emoji);
  if (trimmed.empty()) {
    return Error("Empty reaction");
  }
  auto has_target = store.HasMessageId(thread_id, target_message_id);
  if (!has_target) {
    return has_target.error();
  }
  if (!*has_target) {
    return Error("Target message not found");
  }

  SendRelayOptions opts;
  opts.content_type = ChatContentType::Annotation;
  opts.payload_json = BuildReactionPayloadJson(annotation_type, target_message_id, trimmed);
  opts.update_preview = false;
  const std::string display_text = (annotation_type == kAnnotationTypeReactionClear) ? std::string{} : trimmed;

  auto thread = store.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind == ThreadKind::Group) {
    return mesh_messaging.SendGroupMessage(thread_id, display_text, opts);
  }
  return mesh_messaging.SendUserMessage(thread_id, display_text, opts);
}

} // namespace

Roe<ThreadMessage> MeshMessagingService::SendReaction(const std::string& thread_id,
                                                     const std::string& target_message_id,
                                                     const std::string& emoji) {
  return SendAnnotationReaction(*this, store_, thread_id, target_message_id, emoji, kAnnotationTypeReaction);
}

Roe<ThreadMessage> MeshMessagingService::ClearReaction(const std::string& thread_id,
                                                      const std::string& target_message_id,
                                                      const std::string& emoji) {
  return SendAnnotationReaction(*this, store_, thread_id, target_message_id, emoji, kAnnotationTypeReactionClear);
}

void MeshMessagingService::RetryFailedOutbound() {
  std::vector<PendingRelaySend> pending;
  {
    std::lock_guard lock(retry_mutex_);
    pending.swap(retry_queue_);
  }
  if (pending.empty()) {
    return;
  }

  pending.erase(std::remove_if(pending.begin(), pending.end(),
                               [this](const PendingRelaySend& item) { return IsThreadCompromised(item.thread_id); }),
                pending.end());
  if (pending.empty()) {
    return;
  }

  AppRuntime::PostWorkerNormal([this, pending = std::move(pending)]() mutable {
    if (!relay_) {
      std::lock_guard lock(retry_mutex_);
      retry_queue_.insert(retry_queue_.end(), std::make_move_iterator(pending.begin()),
                          std::make_move_iterator(pending.end()));
      return;
    }
    std::vector<PendingRelaySend> still_pending;
    for (PendingRelaySend& item : pending) {
      if (IsThreadCompromised(item.thread_id)) {
        continue;
      }
      if (item.envelope.recipient_contact_id && direct_chat_ &&
          direct_chat_->IsPeerReachable(*item.envelope.recipient_contact_id)) {
        if (direct_chat_->SendEnvelope(*item.envelope.recipient_contact_id, item.envelope)) {
          ApplySendResult(item.thread_id, item.message_id, true, {}, MessageTransport::Direct);
          continue;
        }
        const auto result = relay_->Send(item.envelope);
        if (result) {
          ApplySendResult(item.thread_id, item.message_id, true, {}, MessageTransport::Relay, true);
          continue;
        }
        if (item.attempt_count < kMaxOutboxRetryAttempts) {
          item.attempt_count += 1;
          still_pending.push_back(std::move(item));
          continue;
        }
        ApplySendResult(item.thread_id, item.message_id, false, result.error().message);
        continue;
      }
      const auto result = relay_->Send(item.envelope);
      if (result) {
        ApplySendResult(item.thread_id, item.message_id, true, {}, MessageTransport::Relay);
        continue;
      }
      if (item.attempt_count < kMaxOutboxRetryAttempts) {
        item.attempt_count += 1;
        still_pending.push_back(std::move(item));
        continue;
      }
      ApplySendResult(item.thread_id, item.message_id, false, result.error().message);
    }
    if (!still_pending.empty()) {
      std::lock_guard lock(retry_mutex_);
      retry_queue_.insert(retry_queue_.end(), std::make_move_iterator(still_pending.begin()),
                          std::make_move_iterator(still_pending.end()));
    }
  });
}

void MeshMessagingService::PollAndMerge() {
  SyncInboxFromWake(false);
}

namespace {

std::string FormatUnixMsIso8601Utc(const int64_t unix_ms) {
  const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
  std::tm tm_utc{};
  if (!os::UtcTime(seconds, &tm_utc)) {
    return {};
  }
  std::ostringstream oss;
  oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
  oss << '.' << std::setw(3) << std::setfill('0') << (unix_ms % 1000) << 'Z';
  return oss.str();
}

} // namespace

Roe<RelayDeleteResult> MeshMessagingService::ClearUndeliveredOlderThan(const int older_than_days) {
  if (!relay_) {
    return Error("Relay client unavailable");
  }
  if (older_than_days <= 0) {
    return Error("older_than_days must be positive");
  }
  auto identity = identity_.Get();
  if (!identity) {
    return Error("Identity unavailable");
  }
  const int64_t before_ms = util::NowUnixMs() - static_cast<int64_t>(older_than_days) * 24LL * 60 * 60 * 1000;
  return relay_->ClearInbox(identity->relay_user_id, FormatUnixMsIso8601Utc(before_ms));
}

void MeshMessagingService::SyncInboxFromWake(const bool /*force*/) {
  RetryFailedOutbound();

  last_relay_poll_ms_ = util::NowUnixMs();
  // Always mark a poll request. If HTTP is already in flight (Accept Sync before MediaKey
  // lands), coalesced callers used to no-op and drop — tablet then never saw CallMediaKey.
  poll_again_.store(true);

  // HTTP PollInbox must NOT run on Browser IO — a 30s curl wait starved AcceptInvite / N025
  // Wire / MediaKey ingest on Samsung (PostAcceptInvite queued, never entered).
  AppRuntime::PostWorkerBackground([this]() {
    bool expected = false;
    if (!poll_pending_.compare_exchange_strong(expected, true)) {
      return;
    }
    struct PollGuard {
      std::atomic<bool>& pending;
      ~PollGuard() { pending = false; }
    } guard{poll_pending_};

    while (poll_again_.exchange(false)) {
      if (!relay_) {
        return;
      }
      auto identity = identity_.Get();
      if (!identity) {
        return;
      }
      std::string cursor = relay_cursor_;
      if (cursor.empty()) {
        LoadPersistedRelayCursor(identity->relay_user_id);
        cursor = relay_cursor_;
      }
      auto poll = relay_->PollInbox(identity->relay_user_id, cursor);
      if (!poll) {
        log().warning << "PollInbox failed: " << poll.error().message;
        brief_relay_health_.store(static_cast<int>(BriefRelayHealthState::Failed),
                                  std::memory_order_relaxed);
        continue;
      }
      brief_relay_health_.store(static_cast<int>(BriefRelayHealthState::Ok), std::memory_order_relaxed);
      std::string next_cursor = poll->next_cursor;
      if (poll->messages.size() > kMaxPollBatchMessages) {
        log().warning << "PollInbox batch too large n=" << poll->messages.size() << " — skip advance";
        continue;
      }
      // Advance cursor on the poll worker before re-poll so a follow-up HTTP does not
      // re-fetch the same batch while ingest is still queued on Browser IO.
      if (!next_cursor.empty()) {
        relay_cursor_ = next_cursor;
        PersistRelayCursor(identity->relay_user_id);
      }
      auto messages = std::move(poll->messages);
      const std::string local_account_id = identity->account_id;
      if (!messages.empty()) {
        log().info << "PollInbox ok n=" << messages.size()
                      << " cursor_advanced=" << (next_cursor.empty() ? 0 : 1);
      }

      // Front of IO so ingest is not stuck behind other long work; HTTP already finished.
      AppRuntime::PostWorkerCritical( [this, local_account_id, local_relay_id = identity->relay_user_id,
                                                         messages = std::move(messages)]() mutable {
        bool changed = false;
        struct UnreadNotice {
          std::string title;
          std::string body;
          std::string thread_id;
        };
        std::vector<UnreadNotice> background_notices;
        for (const RelayEnvelope& envelope : messages) {
          RememberRouteFromEnvelope(envelope);
          const RelayReceiveOutcome outcome = receive_pipeline_->ProcessEnvelope(envelope, local_account_id);
          MaybePublishMemberJoinedAfterIngest(groups_, outcome);
          MaybeSurfaceReceiveFailure(outcome);
          if (outcome.decision == IngestDecision::AcceptGap) {
            const DirectChatTarget inbound_target = InboundTargetFromEnvelope(envelope);
            auto thread = store_.FindDirectThread(inbound_target);
            if (thread && *thread) {
              MaybeRepairGap((*thread)->id, envelope);
            }
          }
          if (!outcome.persisted) {
            continue;
          }
          changed = true;
          if (outcome.thread_id.empty()) {
            continue;
          }
          const std::string& resolved_thread_id = outcome.thread_id;
          std::optional<std::string> preview;
          if (auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64)) {
            preview = decoded->text;
          }
          inbox_.OnInboundMessagePersisted(resolved_thread_id, preview);
          MaybeEnqueueAttachmentDownload(envelope, resolved_thread_id);
          if (!AppLifecycle::IsUserAttentive() && inbox_.ActiveThreadId() != resolved_thread_id) {
            std::string notice_title = "New message";
            if (auto thread = store_.GetThread(resolved_thread_id)) {
              if (*thread) {
                notice_title = inbox_.ResolveThreadLabel(**thread).title;
                if (notice_title.empty()) {
                  notice_title = "New message";
                }
              }
            }
            background_notices.push_back(UnreadNotice{
                .title = std::move(notice_title),
                .body = preview && !preview->empty() ? *preview : "You have a new message",
                .thread_id = resolved_thread_id,
            });
          }
        }

        if (!relay_cursor_.empty() && relay_) {
          // Ack off Browser IO — another HTTP round-trip must not stall the queue.
          const std::string ack_cursor = relay_cursor_;
          const std::string ack_user = local_relay_id;
          IRelayClient* relay = relay_;
          AppRuntime::PostWorkerNormal([relay, ack_user, ack_cursor]() {
            if (!relay) {
              return;
            }
            auto ack = relay->AckInbox(ack_user, ack_cursor);
            if (!ack) {
              logging::getLogger("MeshMessagingService").warning
                  << "Relay inbox ack failed: " << ack.error().message;
            }
          });
        }

        if (changed && on_messages_changed_) {
          AppRuntime::PostUI([this]() { on_messages_changed_(); });
        }
        if (!background_notices.empty() && on_background_unread_) {
          AppRuntime::PostUI(
                                  [this, notices = std::move(background_notices)]() mutable {
                                    for (auto& notice : notices) {
                                      on_background_unread_(std::move(notice.title),
                                                            std::move(notice.body),
                                                            std::move(notice.thread_id));
                                    }
                                  });
        }
      });
    }
  });
}

void MeshMessagingService::SetAttachmentDownloads(AttachmentDownloadService* downloads) {
  attachment_downloads_ = downloads;
  if (chat_sync_) {
    chat_sync_->SetAttachmentDownloads(downloads);
  }
}

void MeshMessagingService::MaybeEnqueueAttachmentDownload(const RelayEnvelope& envelope,
                                                           const std::string& thread_id) {
  if (!attachment_downloads_) {
    return;
  }
  auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64);
  if (!decoded || decoded->content_type != ChatContentType::Attachment) {
    return;
  }
  ThreadMessage message;
  message.id = envelope.message_id;
  message.thread_id = thread_id;
  message.content_type = ChatContentType::Attachment;
  message.payload_json = decoded->payload_json;
  attachment_downloads_->EnqueueFromMessage(thread_id, message);
}

void MeshMessagingService::NotifyMessagesChanged() {
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() {
      if (on_messages_changed_) {
        on_messages_changed_();
      }
    });
  }
}

void MeshMessagingService::SetOnMessagesChanged(std::function<void()> callback) {
  on_messages_changed_ = std::move(callback);
}

void MeshMessagingService::SetOnDeliveryNotice(std::function<void(const std::string&)> callback) {
  on_delivery_notice_ = std::move(callback);
}

void MeshMessagingService::SetOnBackgroundUnread(
    std::function<void(std::string title, std::string body, std::string thread_id)> callback) {
  on_background_unread_ = std::move(callback);
}

} // namespace pbr
