#include "lib/people/ContactTypes.h"
#include "domain/peers/LocalKeysFromIdentity.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/attachments/AmpChatBlobTransport.h"
#include "domain/delivery/AmpChatHistoryTransport.h"
#include "domain/delivery/AmpDirectChatTransport.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "lib/messaging/PublicPskLockCoordinator.h"

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/Utilities.h"
#include "lib/people/DirectChatTargetFromContact.h"
#include "lib/people/PeerDisplayLabel.h"
#include "lib/pricing/InitiationBillingCodec.h"
#include "lib/pricing/InitiationPricing.h"
#include "foundation/data/PricingTypes.h"
#include "lib/people/MeshHopPolicy.h"
#include "mesh/reachability/AmpObservedAddrs.h"
#include "mesh/reachability/Reachability.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/E2eIntegrityUtil.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "lib/messaging/E2ePublicSessionLogic.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/ControlLaneEnvelope.h"
#include "lib/messaging/EnvelopeSigner.h"
#include "lib/messaging/SqliteThreadSchema.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "lib/messaging/ReactionTypes.h"
#include "lib/messaging/PskRotateCodec.h"
#include "lib/messaging/OutboundMessage.h"
#include "lib/messaging/SendRelayOptions.h"
#include "common/chat/MessagingLimits.h"
#include "common/EmojiKey.h"
#include "common/chat/RelayStreamKey.h"
#include "lib/messaging/RelayWirePayload.h"
#include "lib/people/PeerBriefRoute.h"
#include "common/thread/SyncStateTypes.h"
#include "mesh/host/MeshPorts.h"
#include "lib/net/RelayInboxCursor.h"
#include "lib/people/ContactIdentity.h"
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
#include "common/MeshAlias.h"

namespace pbr {
namespace {

/** Mark a message whose send could not even start as failed; returns the error to pass up. */
Error FailOutbound(IThreadStore& store, ThreadMessage& message, Error error) {
  message.delivery = MessageDelivery::Failed;
  (void)store.UpdateMessage(message);
  return error;
}

} // namespace

namespace {

DirectChatTarget InboundTargetFromEnvelope(const RelayEnvelope& envelope) {
  DirectChatTarget target;
  target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  target.peer_identity_value = envelope.sender_contact_id;
  target.channel = envelope.route.channel;
  return target;
}

} // namespace

MeshDeliveryOrchestrator::MeshDeliveryOrchestrator(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                         IRelayClient* relay, InboxController& inbox,
                                         PeerSigningKeyStore& signing_key_store,
                                         IPeerSigningKeyResolver& signing_key_resolver, PeerKemKeyStore& kem_key_store,
                                         IPeerKemKeyResolver& kem_key_resolver, IPskSessionStore& psk_store,
                                         GroupRosterStore& group_roster, GroupInviteGate* invite_gate,
                                         mesh::IChatPeerLinks* amp_links, std::function<void()> amp_io_pump,
                                         std::function<void(std::function<void()>)> amp_worker_post,
                                         std::function<void(std::function<void()>)> amp_post_io,
                                         std::function<void(std::chrono::milliseconds, std::function<void()>)> amp_post_after)
    : store_(store), contacts_(contacts), identity_(identity), relay_(relay), inbox_(inbox),
      signing_key_store_(signing_key_store), signing_key_resolver_(signing_key_resolver), kem_key_store_(kem_key_store),
      kem_key_resolver_(kem_key_resolver), psk_store_(psk_store), group_roster_(group_roster),
      transports_(store, identity, signing_key_resolver, psk_store), outbox_([this]() { return transports_.DirectChat(); }),
      epoch_coordinator_(store), psk_coordinator_(store, psk_store), public_lock_(store, psk_store), routes_(contacts) {
  redirectLogger("MeshDeliveryOrchestrator");
  receive_pipeline_ =
      std::make_unique<RelayReceivePipeline>(store_, signing_key_resolver_, psk_store_, LocalKeysFromIdentity(identity_), group_roster_,
                                             invite_gate);
  receive_pipeline_->SetContactTrust(&contacts_);
  // A peer withdrew a message we hold: its bytes, summary and preview follow (the pipeline wiped the row).
  receive_pipeline_->SetOnMessageRetracted(
      [this](const std::string& thread_id, const ThreadMessage& original) { inbox_.AfterMessageDeleted(thread_id, original); });
  transports_.SetInboundHandler([this](RelayEnvelope envelope) { HandleDirectInbound(std::move(envelope)); });
  outbox_.SetRelayClient(relay_);
  // On the worker that finished the send: the row is marked before the UI hears of it.
  outbox_.SetOnOutcome([this](const DeliveryOutcome& outcome) {
    if (outcome.ok) {
      ApplySendSucceeded(outcome.thread_id, outcome.message_id, outcome.transport, outcome.relay_after_direct);
    } else {
      ApplySendFailed(outcome.thread_id, outcome.message_id, outcome.failure, outcome.detail);
    }
  });
  outbox_.SetThreadPaused([this](const std::string& thread_id) { return IsThreadCompromised(thread_id); });
  // Blob + chat/history: Amp single entry ([A020] / D10). May also AttachAmpTransports later.
  if (amp_links) {
    AttachAmpTransports(amp_links, std::move(amp_io_pump), std::move(amp_worker_post), std::move(amp_post_io),
                        std::move(amp_post_after));
  } else {
    log().warning << "direct chat/history/blob unavailable (Amp required)";
  }
  chat_sync_ = std::make_unique<ChatSyncWorkflow>(store_, identity_, contacts_, relay_, *receive_pipeline_, inbox_,
                                                 transports_.History());
  chat_sync_->SetOnMessagesChanged([this]() {
    if (on_messages_changed_) {
      AppRuntime::PostUI([this]() { on_messages_changed_(); });
    }
  });
}

void MeshDeliveryOrchestrator::RegisterPeerSigningKey(const std::string& peer_identity_kind,
                                               const std::string& peer_identity_value,
                                               const std::string& signing_public_key_b64, const std::string& source) {
  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = signing_public_key_b64;
  record.source = source;
  signing_key_store_.Put(peer_identity_kind, peer_identity_value, std::move(record));
}

void MeshDeliveryOrchestrator::RegisterPeerKemKey(const std::string& peer_identity_kind,
                                           const std::string& peer_identity_value,
                                           const std::string& kem_public_key_b64, const std::string& source) {
  PeerKemKeyRecord record;
  record.kem_public_key_b64 = kem_public_key_b64;
  record.source = source;
  kem_key_store_.Put(peer_identity_kind, peer_identity_value, std::move(record));
}

void MeshDeliveryOrchestrator::SetRelayClient(IRelayClient* relay) {
  relay_ = relay;
  outbox_.SetRelayClient(relay);
  relay_cursor_.clear();
  poll_pending_ = false;
  poll_again_ = false;
  if (auto identity = identity_.Get()) {
    LoadPersistedRelayCursor(identity->relay_user_id);
  }
  if (chat_sync_) {
    chat_sync_ = std::make_unique<ChatSyncWorkflow>(store_, identity_, contacts_, relay_, *receive_pipeline_, inbox_,
                                                 transports_.History());
    chat_sync_->SetOnMessagesChanged([this]() {
      if (on_messages_changed_) {
        AppRuntime::PostUI([this]() { on_messages_changed_(); });
      }
    });
  }
  TailSyncActiveE2eThread();
}

void MeshDeliveryOrchestrator::SetInitiationBillingStore(InitiationBillingStore* store) {
  initiation_billing_ = store;
}

void MeshDeliveryOrchestrator::SetSystemControls(const SystemControlRegistry* controls) {
  controls_ = controls;
  if (receive_pipeline_) {
    receive_pipeline_->SetSystemControls(controls_);
  }
}

void MeshDeliveryOrchestrator::SetGroupMembership(GroupMembershipWorkflow* groups) {
  groups_ = groups;
}

void MeshDeliveryOrchestrator::SetProfileDataDir(std::string profile_data_dir) {
  profile_data_dir_ = std::move(profile_data_dir);
  control_lane_seen_ = std::make_unique<ControlLaneSeenStore>(SqliteThreadProfileDbFile(profile_data_dir_));
  if (receive_pipeline_) {
    receive_pipeline_->SetControlLaneSeen(control_lane_seen_.get());
  }
  transports_.SetProfileDataDir(profile_data_dir_);
}

IChatBlobPeerClient* MeshDeliveryOrchestrator::PeerBlobClient() const {
  return transports_.BlobClient();
}

IChatBlobPeerService* MeshDeliveryOrchestrator::PeerBlobService() const {
  return transports_.BlobService();
}

void MeshDeliveryOrchestrator::LoadPersistedRelayCursor(const std::string& relay_user_id) {
  if (profile_data_dir_.empty()) {
    return;
  }
  const std::string loaded = LoadRelayInboxCursor(profile_data_dir_, relay_user_id);
  if (!loaded.empty()) {
    relay_cursor_ = loaded;
  }
}

void MeshDeliveryOrchestrator::PersistRelayCursor(const std::string& relay_user_id) {
  if (profile_data_dir_.empty() || relay_cursor_.empty()) {
    return;
  }
  SaveRelayInboxCursor(profile_data_dir_, relay_user_id, relay_cursor_);
}

void MeshDeliveryOrchestrator::RegisterPeerDirectEndpoint(const std::string& peer_relay_user_id,
                                                     const std::string& multiaddr) {
  transports_.RegisterEndpoint(peer_relay_user_id, multiaddr);
}

void MeshDeliveryOrchestrator::RegisterPeerDirectEndpoints(
    const std::string& peer_relay_user_id, const std::vector<std::string>& multiaddrs) {
  transports_.RegisterEndpoints(peer_relay_user_id, multiaddrs);
}

void MeshDeliveryOrchestrator::RegisterContactDirectEndpoints(const Contact& contact) {
  transports_.RegisterContactEndpoints(contact);
}

void MeshDeliveryOrchestrator::SetPeerRouteSources(DirectoryShadowCache* shadows, mesh::IDirectoryClient* directory) {
  routes_.SetSources(shadows, directory);
}

void MeshDeliveryOrchestrator::NoteAccountRelayRoute(const std::string& account_id,
                                                const std::string& relay_user_id) {
  routes_.NoteAccountRoute(account_id, relay_user_id);
}

namespace {

void MaybePublishMemberJoinedAfterIngest(GroupMembershipWorkflow* groups, const RelayReceiveOutcome& outcome) {
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

void MeshDeliveryOrchestrator::MaybeSurfaceReceiveFailure(const RelayReceiveOutcome& outcome) {
  if (!outcome.receive_failure) {
    return;
  }
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::ReceiveFailed;
  notice.receive = *outcome.receive_failure;
  notice.peer = outcome.receive_failure_sender.value_or("");
  notice.family = outcome.receive_failure_family;
  notice.detail = outcome.receive_failure_detail;
  log().warning << "Inbound receive failure sender=" << notice.peer << " reason=" << ReceiveFailureWire(notice.receive)
                << " family=" << notice.family << " detail=" << notice.detail;

  // One toast/system line per sender+reason within the cooldown — old relay backlog must not flood UI.
  const std::string rate_key = notice.peer + "|" + ReceiveFailureWire(notice.receive) + "|" + notice.family;
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
    // No stored words: the transcript words the line from its payload, in the reader's language.
    Object detail;
    detail.set("sender", notice.peer);
    detail.set("reason", ReceiveFailureWire(notice.receive));
    detail.set("family", notice.family);
    detail.set("detail", notice.detail);
    Object payload;
    payload.set("control_type", "receive_failure");
    payload.set("detail", DumpJson(detail));
    local.payload_json = DumpJson(payload);
    local.timestamp = now_ms;
    local.delivery = MessageDelivery::Local;
    local.relay_visible = false;
    local.generation = "local";
    if (store_.AppendMessage(local)) {
      inbox_.OnInboundMessagePersisted(*outcome.receive_failure_thread_id);
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

void MeshDeliveryOrchestrator::HandleDirectInbound(RelayEnvelope envelope) {
  auto identity = identity_.Get();
  if (!identity) {
    return;
  }
  const RelayReceiveOutcome outcome =
      receive_pipeline_->ProcessEnvelope(envelope, identity->account_id, false, MessageTransport::Direct);
  routes_.RememberFromEnvelope(envelope, outcome);
  MaybePublishMemberJoinedAfterIngest(groups_, outcome);
  MaybeSurfaceReceiveFailure(outcome);
  if (outcome.decision == IngestDecision::AcceptGap) {
    auto thread = store_.FindDirectThread(InboundTargetFromEnvelope(envelope));
    if (thread && *thread) {
      MaybeRepairGap((*thread)->id, envelope);
    }
  }
  if (outcome.persisted && !outcome.thread_id.empty() && !outcome.deleted_by_sender) {
    std::optional<std::string> preview;
    if (outcome.persisted_message) {
      preview = outcome.persisted_message->text;
    }
    inbox_.OnInboundMessagePersisted(outcome.thread_id, preview);
    MaybeEnqueueAttachmentDownload(outcome);
  }
  if (outcome.persisted || outcome.thread_changed) {
    if (on_messages_changed_) {
      AppRuntime::PostUI([this]() { on_messages_changed_(); });
    }
  }
}

bool MeshDeliveryOrchestrator::AmpReaches(const std::string& peer_key) const {
  IDirectMessageClient* chat = transports_.DirectChat();
  return chat && chat->IsPeerReachable(peer_key);
}

void MeshDeliveryOrchestrator::TickMesh() {
  // Amp mesh pump is MeshHost::Tick via ConversationsHub; no PeerSessionManager.
}

void MeshDeliveryOrchestrator::WarmPeerForThread(const std::string& thread_id) {
  if (!transports_.Attached()) {
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
  WarmPeerByKey(peer);
}

void MeshDeliveryOrchestrator::WarmPeerByKey(const std::string& peer_key) {
  transports_.WarmByKey(peer_key);
}

Roe<MeshDeliveryOrchestrator::E2ePublicSession> MeshDeliveryOrchestrator::ResolveE2ePublicSession(
    const std::string& peer_identity) {
  if (peer_identity.empty()) {
    return Error("Peer identity required");
  }
  E2ePublicSession session;
  session.target_key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  session.target_key.peer_identity_value = peer_identity;
  session.target_key.channel = CryptoChannel::E2ePublic;
  // The pair's session is the account pair's, not a thread's: its active epoch is the stored one.
  auto record = psk_store_.Load(session.target_key);
  if (!record) {
    return record.error();
  }
  session.epoch = record->has_value() && (*record)->session_epoch > 0 ? (*record)->session_epoch : 1;
  auto peer_public = PeerKemPublicKey(session.target_key);
  if (!peer_public) {
    return peer_public.error();
  }
  auto ensured = EnsureE2ePublicMasterPsk(psk_store_, session.target_key, session.epoch, *peer_public);
  if (!ensured) {
    return ensured.error();
  }
  session.master_psk = std::move(ensured->master_psk);
  session.key_init_b64 = std::move(ensured->key_init_b64);
  return session;
}

Roe<EnsuredE2ePublicSessionKey> MeshDeliveryOrchestrator::EnsureE2ePublicSessionKey(
    const std::string& peer_identity) {
  auto session = ResolveE2ePublicSession(peer_identity);
  if (!session) {
    return session.error();
  }
  auto session_key = DeriveE2ePublicSessionKey(session->master_psk, session->epoch);
  if (!session_key) {
    return session_key.error();
  }
  EnsuredE2ePublicSessionKey out;
  out.session_key = std::move(*session_key);
  if (session->key_init_b64 && !session->key_init_b64->empty()) {
    out.first_message_key_init_b64 = std::move(*session->key_init_b64);
  }
  return out;
}

Roe<ByteVector> MeshDeliveryOrchestrator::PeerKemPublicKey(const ChatTargetKey& key) const {
  auto kem = kem_key_resolver_.Resolve(key.peer_identity_kind, key.peer_identity_value);
  if (!kem) {
    return kem.error();
  }
  return Base64Decode(kem->kem_public_key_b64);
}

uint64_t MeshDeliveryOrchestrator::NextControlOrderKey() {
  // Relay index within the lane's stream: unique and rising per sender (ms * 1000 + a counter).
  const uint64_t floor = static_cast<uint64_t>(util::NowUnixMs()) * 1000;
  uint64_t current = control_order_key_.load(std::memory_order_relaxed);
  uint64_t next = 0;
  do {
    next = std::max(current + 1, floor);
  } while (!control_order_key_.compare_exchange_weak(current, next, std::memory_order_relaxed));
  return next;
}

Roe<void> MeshDeliveryOrchestrator::SendControl(const std::string& peer_identity, const ThreadMessage& control,
                                                const std::optional<std::string>& key_init_b64, const bool urgent) {
  auto identity = identity_.Get();
  if (!identity || identity->account_id.empty()) {
    return Error("Local account identity unavailable");
  }
  auto session = ResolveE2ePublicSession(peer_identity);
  if (!session) {
    return session.error();
  }
  // A key_init the caller holds (its media-key wrap opened the session) rides this first send.
  std::optional<std::string> key_init = session->key_init_b64;
  if (!key_init && key_init_b64 && !key_init_b64->empty()) {
    key_init = key_init_b64;
  }

  // Route as for a DM with the peer (contacts, learned routes, directory) — no thread needed.
  Thread route_view;
  route_view.kind = ThreadKind::Direct;
  route_view.channel = ThreadChannel::E2ePublic;
  route_view.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  route_view.peer_identity_value = peer_identity;
  if (auto contact = contacts_.FindByIdentity(peer_identity, mesh::ContactIdKind::Account); contact && *contact) {
    route_view.participant_contact_ids = {(*contact)->id};
  }
  const auto peer_relay_id = routes_.ResolveWithDirectory(route_view);
  const bool amp_reachable = AmpReaches(peer_identity);
  if (!peer_relay_id && !amp_reachable) {
    return Error("No route to the peer");
  }

  ControlLaneSendParams params;
  params.sender_account_id = identity->account_id;
  params.sender_relay_id = identity->relay_user_id;
  params.peer_account_id = peer_identity;
  params.route_party = peer_relay_id ? *peer_relay_id : peer_identity;
  params.session_epoch = session->epoch;
  params.master_psk = std::move(session->master_psk);
  params.key_init_b64 = std::move(key_init);
  params.order_key = NextControlOrderKey();
  auto built = BuildControlLaneEnvelope(params, control, [this](const std::vector<uint8_t>& bytes) {
    return identity_.SignBytes(bytes);
  });
  if (!built) {
    return built.error();
  }
  RelayEnvelope envelope = std::move(*built);
  envelope.relay_wake_urgent = urgent;
  const std::string built_message_id = envelope.message_id;

  // Off the caller's thread, on the critical lane (signaling must not queue behind inbox polls); the
  // relay first when a route is known (Amp's open + ack per message races media setup). No row to
  // mark: a relay failure is retried while fresh.
  outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                               .message_id = built_message_id,
                               .amp_peer_key = peer_identity,
                               .relay_route = peer_relay_id,
                               .critical_lane = true});
  return {};
}

ThreadPeerLinkView MeshDeliveryOrchestrator::GetThreadPeerLink(const std::string& thread_id) const {
  ThreadPeerLinkView view;
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Direct) {
    return view;
  }
  const std::string& peer = (*thread)->peer_identity_value;
  // The same route a send would use (contacts, learned from verified senders, the directory cache).
  view.relay_available = relay_ != nullptr && routes_.Resolve(**thread).has_value();
  mesh::IChatPeerLinks* links = transports_.Links();
  if (peer.empty() || !links) {
    view.path_kind = view.relay_available && !peer.empty() ? ThreadPeerPathKind::ViaRelay : ThreadPeerPathKind::Failed;
    return view;
  }
  const mesh::MeshPeerLinkSnapshot snap = links->GetLinkSnapshot(peer);
  view.phase = snap.phase;
  view.has_direct_endpoint = snap.has_endpoint;
  view.carrier_backed = snap.carrier_backed;
  view.backoff_seconds = static_cast<int>((snap.backoff_remaining.count() + 999) / 1000);
  view.detail = snap.detail;
  switch (snap.phase) {
  case mesh::MeshPeerLinkPhase::Connected:
    view.path_kind = snap.carrier_backed ? ThreadPeerPathKind::ViaHop : ThreadPeerPathKind::Direct;
    break;
  case mesh::MeshPeerLinkPhase::Dialing:
  case mesh::MeshPeerLinkPhase::Handshaking:
    view.path_kind = ThreadPeerPathKind::Connecting;  // includes a punch burst: never a settled badge
    break;
  case mesh::MeshPeerLinkPhase::Backoff:
    view.path_kind = ThreadPeerPathKind::Degraded;
    view.show_retry = true;
    break;
  case mesh::MeshPeerLinkPhase::Idle:
    view.path_kind = ThreadPeerPathKind::Ready;
    break;
  case mesh::MeshPeerLinkPhase::Unavailable:
  default:
    view.path_kind = view.relay_available ? ThreadPeerPathKind::ViaRelay : ThreadPeerPathKind::Failed;
    break;
  }
  return view;
}

void MeshDeliveryOrchestrator::RetryPeerDial(const std::string& thread_id) {
  if (!transports_.Attached()) {
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
  WarmPeerByKey(peer);
}

bool MeshDeliveryOrchestrator::IsE2ePrivateThread(const std::string& thread_id) const {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread) {
    return false;
  }
  return (*thread)->kind == ThreadKind::Direct && (*thread)->channel == ThreadChannel::E2e;
}

bool MeshDeliveryOrchestrator::IsThreadCompromised(const std::string& thread_id) const {
  return IsE2ePrivateThread(thread_id) && IsE2eThreadCompromised(store_, thread_id);
}

bool MeshDeliveryOrchestrator::IsPskReadyToSend(const std::string& thread_id) const {
  if (!IsE2ePrivateThread(thread_id)) {
    return true;
  }
  auto status = psk_coordinator_.GetStatus(thread_id);
  return status && status->has_psk && status->verified;
}

std::optional<std::string> MeshDeliveryOrchestrator::LiveExchange() const {
  return controls_ ? controls_->LiveFamily() : std::nullopt;
}

Roe<void> MeshDeliveryOrchestrator::MaybeSendPublicAutoRekey(const std::string& thread_id) {
  if (LiveExchange()) {
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

Roe<ThreadMessage> MeshDeliveryOrchestrator::SendPublicPskRotate(const std::string& thread_id,
                                                           const PublicPskRotateKind kind) {
  if (const auto live = LiveExchange()) {
    return Error("Finish the " + *live + " first");
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
    auto decoded = PeerKemPublicKey(E2eRelayPayloadCodec::ChatTargetFromThread(**thread));
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

Roe<void> MeshDeliveryOrchestrator::LockPublicThreadToThisDevice(const std::string& thread_id) {
  auto sent = SendPublicPskRotate(thread_id, PublicPskRotateKind::Lock);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<PublicKeyScope> MeshDeliveryOrchestrator::GetPublicKeyScope(const std::string& thread_id) const {
  return public_lock_.GetKeyScope(thread_id);
}

Roe<bool> MeshDeliveryOrchestrator::CanLockPublicToThisDevice(const std::string& thread_id) const {
  if (!support_account_id_.empty()) {
    auto thread = store_.GetThread(thread_id);
    if (thread && *thread && (*thread)->peer_identity_value == support_account_id_) {
      return false;
    }
  }
  return public_lock_.CanLockToThisDevice(thread_id);
}

void MeshDeliveryOrchestrator::SetSupportAccountId(std::string account_id) {
  support_account_id_ = std::move(account_id);
}

void MeshDeliveryOrchestrator::PurgeRetryQueueForThread(const std::string& thread_id) {
  outbox_.PurgeThread(thread_id);
}

Roe<uint32_t> MeshDeliveryOrchestrator::StartNewSecureChat(const std::string& thread_id) {
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

Roe<void> MeshDeliveryOrchestrator::PauseIntegrityOnly(const std::string& thread_id) {
  if (!IsE2ePrivateThread(thread_id)) {
    return Error("Not a private E2E thread");
  }
  PurgeRetryQueueForThread(thread_id);
  return epoch_coordinator_.PauseOnly(thread_id);
}

Roe<std::string> MeshDeliveryOrchestrator::RotatePskAndExportBundle(const std::string& thread_id) {
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

Roe<PskSessionStatus> MeshDeliveryOrchestrator::GetPskStatus(const std::string& thread_id) const {
  return psk_coordinator_.GetStatus(thread_id);
}

Roe<PskExportView> MeshDeliveryOrchestrator::EnsurePskGenerated(const std::string& thread_id) {
  return psk_coordinator_.EnsureGenerated(thread_id);
}

Roe<PskExportView> MeshDeliveryOrchestrator::GetPskExportView(const std::string& thread_id) const {
  return psk_coordinator_.GetExportView(thread_id);
}

Roe<std::string> MeshDeliveryOrchestrator::ExportPskBundleJson(const std::string& thread_id) const {
  return psk_coordinator_.ExportBundleJson(thread_id);
}

Roe<void> MeshDeliveryOrchestrator::ImportPskRawBase64(const std::string& thread_id, const std::string& raw_b64) {
  return psk_coordinator_.ImportRawBase64(thread_id, raw_b64);
}

Roe<void> MeshDeliveryOrchestrator::ImportPskBundleJson(const std::string& thread_id, const std::string& bundle_json) {
  return psk_coordinator_.ImportBundleJson(thread_id, bundle_json);
}

Roe<void> MeshDeliveryOrchestrator::MarkPskVerified(const std::string& thread_id) {
  return psk_coordinator_.MarkVerified(thread_id, util::NowUnixMs());
}

void MeshDeliveryOrchestrator::ScrollBackfill(const std::string& thread_id,
                                         std::function<void(Roe<ChatSyncResult>)> on_complete) {
  RunSyncOnIo(thread_id,
              [this, thread_id](std::function<void(Roe<ChatSyncResult>)> done) {
                chat_sync_->ScrollBackfillAsync(thread_id, std::move(done));
              },
              std::move(on_complete));
}

void MeshDeliveryOrchestrator::TailSyncActiveE2eThread() {
  const std::string& active_id = inbox_.ActiveThreadId();
  if (active_id.empty() || !IsE2ePrivateThread(active_id)) {
    return;
  }
  MaybeTailSync(active_id);
}

void MeshDeliveryOrchestrator::RunSyncOnIo(
    const std::string& thread_id, std::function<void(std::function<void(Roe<ChatSyncResult>)>)> task,
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

  AppRuntime::PostWorkerNormal([this, task = std::move(task), on_complete = std::move(on_complete)]() mutable {
    task([this, on_complete = std::move(on_complete)](Roe<ChatSyncResult> result) mutable {
      sync_pending_.store(false, std::memory_order_release);
      if (result && on_messages_changed_) {
        AppRuntime::PostUI([this]() { on_messages_changed_(); });
      }
      if (on_complete) {
        AppRuntime::PostUI([on_complete = std::move(on_complete), result = std::move(result)]() mutable {
          on_complete(std::move(result));
        });
      }
    });
  });
}

void MeshDeliveryOrchestrator::SyncWithPeer(const std::string& thread_id,
                                       std::function<void(Roe<ChatSyncResult>)> on_complete) {
  RunSyncOnIo(thread_id,
              [this, thread_id](std::function<void(Roe<ChatSyncResult>)> done) {
                chat_sync_->UserInitiatedSyncAsync(thread_id, std::move(done));
              },
              std::move(on_complete));
}

void MeshDeliveryOrchestrator::RetryGapSync(const std::string& thread_id,
                                       std::function<void(Roe<ChatSyncResult>)> on_complete) {
  RunSyncOnIo(thread_id,
              [this, thread_id](std::function<void(Roe<ChatSyncResult>)> done) {
                chat_sync_->RetryGapSyncAsync(thread_id, std::move(done));
              },
              std::move(on_complete));
}

TrustLevel MeshDeliveryOrchestrator::ResolveThreadTrust(const Thread& thread) const {
  if (thread.participant_contact_ids.empty()) {
    return TrustLevel::Unknown;
  }
  const auto contact = contacts_.Get(thread.participant_contact_ids.front());
  if (!contact || !*contact) {
    return TrustLevel::Unknown;
  }
  return (*contact)->trust;
}

void MeshDeliveryOrchestrator::NotifyDeliveryIssue(const Thread& thread, const SendFailure failure,
                                                   const std::string& detail) {
  if (!on_delivery_notice_) {
    return;
  }
  log().info << "send failed thread=" << thread.id << " detail=" << detail;
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::SendFailed;
  notice.send = SendFailureForThread(failure, ResolveThreadTrust(thread) == TrustLevel::Friendly);
  notice.detail = detail;
  AppRuntime::PostUI([this, notice]() {
    if (on_delivery_notice_) {
      on_delivery_notice_(notice);
    }
  });
}

void MeshDeliveryOrchestrator::NotifyRelayFallback(const std::string& /*thread_id*/) {
  if (!on_delivery_notice_) {
    return;
  }
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::SentViaRelay;
  AppRuntime::PostUI([this, notice]() {
    if (on_delivery_notice_) {
      on_delivery_notice_(notice);
    }
  });
}

void MeshDeliveryOrchestrator::MaybeTailSync(const std::string& thread_id) {
  WarmPeerForThread(thread_id);
  if (!chat_sync_) {
    return;
  }
  // SoftMigrate / Accept must not sit behind history TailSync on the Normal worker + relay HTTP.
  if (const auto live = LiveExchange()) {
    log().info << "MaybeTailSync deferred (live " << *live << ") thread=" << thread_id;
    return;
  }
  AppRuntime::PostWorkerNormal([this, thread_id]() {
    chat_sync_->TailSyncAsync(thread_id, [this](Roe<ChatSyncResult> result) {
      if (result && on_messages_changed_) {
        AppRuntime::PostUI([this]() { on_messages_changed_(); });
      }
    });
  });
}

void MeshDeliveryOrchestrator::MaybeRepairGap(const std::string& thread_id, const RelayEnvelope& envelope) {
  if (!chat_sync_ || !envelope.session_epoch) {
    return;
  }
  if (const auto live = LiveExchange()) {
    log().info << "MaybeRepairGap deferred (live " << *live << ") thread=" << thread_id;
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
  AppRuntime::PostWorkerNormal([this, thread_id, gap_min, gap_max]() {
    chat_sync_->RepairGapAsync(thread_id, gap_min, gap_max, [this](Roe<ChatSyncResult> result) {
      if (result && on_messages_changed_) {
        AppRuntime::PostUI([this]() { on_messages_changed_(); });
      }
    });
  });
}

bool MeshDeliveryOrchestrator::MarkDelivery(const std::string& thread_id, const std::string& message_id,
                                            const bool success, const MessageTransport transport) {
  if (thread_id.empty()) {
    return false;  // a control-lane send: no transcript row to update
  }
  auto messages = store_.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!messages) {
    return false;
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
  return true;
}

void MeshDeliveryOrchestrator::ApplySendSucceeded(const std::string& thread_id, const std::string& message_id,
                                                  const MessageTransport transport,
                                                  const bool relay_after_direct_attempt) {
  if (!MarkDelivery(thread_id, message_id, true, transport)) {
    return;
  }
  if (transport == MessageTransport::Relay && relay_after_direct_attempt) {
    NotifyRelayFallback(thread_id);
  }
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
}

void MeshDeliveryOrchestrator::ApplySendFailed(const std::string& thread_id, const std::string& message_id,
                                               const SendFailure failure, const std::string& detail) {
  if (!MarkDelivery(thread_id, message_id, false, MessageTransport::Relay)) {
    return;
  }
  if (auto thread = store_.GetThread(thread_id); thread && *thread) {
    NotifyDeliveryIssue(**thread, failure, detail);
  }
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
}

Roe<ThreadMessage> MeshDeliveryOrchestrator::SendUserMessage(const std::string& thread_id, const std::string& text,
                                                         const SendRelayOptions& options) {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  DirectSendGate gate;
  gate.direct_thread = *thread && (*thread)->kind == ThreadKind::Direct;
  if (gate.direct_thread) {
    gate.channel = (*thread)->channel;
    gate.peer_known = !(*thread)->peer_identity_value.empty();
    if (gate.channel == ThreadChannel::E2e) {
      gate.compromised = IsThreadCompromised(thread_id);
      gate.psk_ready = IsPskReadyToSend(thread_id);
    } else if (gate.channel == ThreadChannel::E2ePublic) {
      auto scope = public_lock_.GetKeyScope(thread_id);
      if (!scope) {
        return scope.error();
      }
      gate.locked_out = *scope == PublicKeyScope::LockedOut;
    }
  }
  if (auto allowed = CheckDirectSendAllowed(gate); !allowed) {
    return allowed.error();
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

  auto appended = AppendOutboundMessage(
      store_, NewOutboundMessage(thread_id, text, options, util::GenerateUuid(), util::NowUnixMs()));
  if (!appended) {
    return appended.error();
  }
  if (options.update_preview) {
    (void)inbox_.UpdatePreview(thread_id, text);
  }

  auto identity = identity_.Get();
  if (!identity) {
    return FailOutbound(store_, *appended, identity.error());
  }
  auto peer_relay_id = routes_.ResolveWithDirectory(**thread);
  const std::string amp_peer_key = (*thread)->peer_identity_value;
  const bool amp_reachable = AmpReaches(amp_peer_key);
  if (!peer_relay_id && !amp_reachable) {
    return FailOutbound(store_, *appended, Error("Direct thread missing peer relay id"));
  }
  auto sealed = SealDirectPayload(*appended, **thread, identity->account_id, options, psk_store_,
                                  [this](const ChatTargetKey& key) { return PeerKemPublicKey(key); });
  if (!sealed) {
    return FailOutbound(store_, *appended, sealed.error());
  }
  // Brief recipient is relay:; Amp-only (no route yet) uses Account as party placeholder.
  RelayEnvelope envelope =
      BuildDirectEnvelope(*appended, EnvelopeSender{identity->relay_user_id, identity->account_id},
                          (*thread)->channel, peer_relay_id ? *peer_relay_id : amp_peer_key, std::move(*sealed));
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  if (!sign_bytes) {
    return FailOutbound(store_, *appended, sign_bytes.error());
  }
  auto signature = identity_.SignBytes(*sign_bytes);
  if (signature) {
    envelope.signature = *signature;
  }

  // Amp dial key is Account ID (invite listen multiaddrs / contact endpoints). Brief uses relay:.
  outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                               .thread_id = thread_id,
                               .message_id = appended->id,
                               .amp_peer_key = amp_peer_key,
                               .relay_route = peer_relay_id,
                               .critical_lane = options.critical_lane});

  // Always hop to UI — SendUserMessage runs on IO during membership fan-out (PublishMemberJoined).
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
  if (!system_control && (*thread)->channel == ThreadChannel::E2ePublic) {
    (void)public_lock_.NoteTraffic(thread_id);
  }
  return *appended;
}

Roe<void> MeshDeliveryOrchestrator::SendChargeRequired(const std::string& peer_identity,
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
  direct_target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = peer_identity;
  if (auto contact = contacts_.FindByIdentity(peer_identity, mesh::ContactIdKind::Account)) {
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

Roe<ThreadMessage> MeshDeliveryOrchestrator::SendGroupMessage(const std::string& thread_id, const std::string& text,
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

  auto appended = AppendOutboundMessage(
      store_, NewOutboundMessage(thread_id, text, options, util::GenerateUuid(), util::NowUnixMs()));
  if (!appended) {
    return appended.error();
  }
  const ThreadMessage& message = *appended;
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

  auto resolve_kem = [this](const ChatTargetKey& key) { return PeerKemPublicKey(key); };

  std::optional<std::vector<uint8_t>> rich_plaintext;
  if (options.content_type && *options.content_type != ChatContentType::Text) {
    auto plaintext = ChatPayloadCodec::EncodeToRow(message);
    if (!plaintext) {
      return FailOutbound(store_, *appended, plaintext.error());
    }
    rich_plaintext = std::move(*plaintext);
  }

  const std::string group_id = *(*thread)->group_id;
  auto encrypted = GroupE2ePayloadCodec::EncryptForMembers(
      text, group_id, identity->account_id, message.id, *message.sender_seq, *message.session_epoch,
      message.timestamp, targets, psk_store_, resolve_kem, rich_plaintext);
  if (!encrypted) {
    if (groups_) {
      for (const GroupMemberTarget& target : targets) {
        groups_->MarkMemberUnreachable(group_id, target.member_identity);
      }
    }
    return FailOutbound(store_, *appended, encrypted.error());
  }

  if (groups_) {
    for (const std::string& failed_id : encrypted->failed_member_identities) {
      groups_->MarkMemberUnreachable(group_id, failed_id);
    }
  }

  const EnvelopeSender sender{identity->relay_user_id, identity->account_id};
  for (const auto& [recipient, payload_b64] : encrypted->member_payloads) {
    std::optional<std::string> key_init;
    if (auto found = encrypted->member_key_inits.find(recipient); found != encrypted->member_key_inits.end()) {
      key_init = found->second;
    }
    RelayEnvelope envelope =
        BuildGroupMemberEnvelope(message, sender, group_id, recipient, payload_b64, std::move(key_init));

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
        DeliveryNotice notice;
        notice.kind = DeliveryNotice::Kind::GroupMembersMissed;
        on_delivery_notice_(notice);
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

Roe<ThreadMessage> SendAnnotationReaction(MeshDeliveryOrchestrator& mesh_messaging, IThreadStore& store, const std::string& thread_id,
                                          const std::string& target_message_id, const std::string& emoji,
                                          const char* annotation_type) {
  if (target_message_id.empty()) {
    return Error("Missing target message");
  }
  const bool retract = std::string(annotation_type) == kAnnotationTypeRetract;
  const std::string trimmed = retract ? std::string() : NormalizeEmojiKey(emoji);
  if (trimmed.empty() && !retract) {
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

Roe<ThreadMessage> MeshDeliveryOrchestrator::SendReaction(const std::string& thread_id,
                                                     const std::string& target_message_id,
                                                     const std::string& emoji) {
  return SendAnnotationReaction(*this, store_, thread_id, target_message_id, emoji, kAnnotationTypeReaction);
}

Roe<ThreadMessage> MeshDeliveryOrchestrator::ClearReaction(const std::string& thread_id,
                                                      const std::string& target_message_id,
                                                      const std::string& emoji) {
  return SendAnnotationReaction(*this, store_, thread_id, target_message_id, emoji, kAnnotationTypeReactionClear);
}

Roe<ThreadMessage> MeshDeliveryOrchestrator::SendRetract(const std::string& thread_id,
                                                        const std::string& target_message_id) {
  return SendAnnotationReaction(*this, store_, thread_id, target_message_id, std::string(), kAnnotationTypeRetract);
}

void MeshDeliveryOrchestrator::CancelPendingSend(const std::string& message_id) {
  outbox_.Cancel(message_id);
}

void MeshDeliveryOrchestrator::RetryFailedOutbound() {
  outbox_.Retry();
}

void MeshDeliveryOrchestrator::PollAndMerge() {
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

Roe<RelayDeleteResult> MeshDeliveryOrchestrator::ClearUndeliveredOlderThan(const int older_than_days) {
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

void MeshDeliveryOrchestrator::SyncInboxFromWake(const bool /*force*/) {
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
          const RelayReceiveOutcome outcome = receive_pipeline_->ProcessEnvelope(envelope, local_account_id);
          routes_.RememberFromEnvelope(envelope, outcome);
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
          if (outcome.suppress_inbox_chrome) {
            // Call MediaKey / SFU / roster — apply side effects only; do not bump sidebar/unread.
            continue;
          }
          std::optional<std::string> preview;
          if (outcome.persisted_message) {
            preview = outcome.persisted_message->text;
          }
          inbox_.OnInboundMessagePersisted(resolved_thread_id, preview);
          MaybeEnqueueAttachmentDownload(outcome);
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
              logging::getLogger("MeshDeliveryOrchestrator").warning
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

void MeshDeliveryOrchestrator::SetAttachmentDownloads(AttachmentFetchWorkflow* downloads) {
  attachment_downloads_ = downloads;
  if (chat_sync_) {
    chat_sync_->SetAttachmentDownloads(downloads);
  }
}

void MeshDeliveryOrchestrator::AttachAmpTransports(mesh::IChatPeerLinks* amp_links, std::function<void()> amp_io_pump,
                                                   std::function<void(std::function<void()>)> amp_worker_post,
                                                   std::function<void(std::function<void()>)> amp_post_io,
                                                   std::function<void(std::chrono::milliseconds, std::function<void()>)> amp_post_after) {
  if (!transports_.Attach(amp_links, std::move(amp_io_pump), std::move(amp_worker_post), std::move(amp_post_io),
                          std::move(amp_post_after))) {
    return;
  }
  if (chat_sync_) {
    chat_sync_->SetPeerHistoryClient(transports_.History());
  }
}

void MeshDeliveryOrchestrator::DetachAmpTransports() {
  if (chat_sync_) {
    chat_sync_->SetPeerHistoryClient(nullptr);
  }
  transports_.Detach();
}

void MeshDeliveryOrchestrator::MaybeEnqueueAttachmentDownload(const RelayReceiveOutcome& outcome) {
  // The stored, decrypted message — never the wire payload (ciphertext on every chat channel).
  if (!attachment_downloads_ || !outcome.persisted_message ||
      outcome.persisted_message->content_type != ChatContentType::Attachment || outcome.thread_id.empty()) {
    return;
  }
  attachment_downloads_->EnqueueFromMessage(outcome.thread_id, *outcome.persisted_message);
}

void MeshDeliveryOrchestrator::NotifyMessagesChanged() {
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() {
      if (on_messages_changed_) {
        on_messages_changed_();
      }
    });
  }
}

void MeshDeliveryOrchestrator::SetOnMessagesChanged(std::function<void()> callback) {
  on_messages_changed_ = std::move(callback);
}

void MeshDeliveryOrchestrator::SetOnDeliveryNotice(std::function<void(const DeliveryNotice&)> callback) {
  on_delivery_notice_ = std::move(callback);
}

void MeshDeliveryOrchestrator::SetOnBackgroundUnread(
    std::function<void(std::string title, std::string body, std::string thread_id)> callback) {
  on_background_unread_ = std::move(callback);
}

} // namespace pbr
