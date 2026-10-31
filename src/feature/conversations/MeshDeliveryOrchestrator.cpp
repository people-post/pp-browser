#include "lib/people/ContactTypes.h"
#include "domain/peers/LocalKeysFromIdentity.h"
#include "feature/conversations/GroupMembershipWorkflow.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/attachments/AmpChatBlobTransport.h"
#include "domain/delivery/AmpChatHistoryTransport.h"
#include "domain/delivery/AmpDirectChatTransport.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"

#include "lib/messaging/TextSpans.h"
#include "lib/messaging/MentionAlert.h"
#include "lib/messaging/GroupRosterSync.h"
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
                                         IRelayClient* relay, InboxController& inbox, AccountKeyResolver& account_keys,
                                         IPskSessionStore& psk_store,
                                         GroupRosterStore& group_roster, GroupInviteGate* invite_gate,
                                         mesh::IChatPeerLinks* amp_links, std::function<void()> amp_io_pump,
                                         std::function<void(std::function<void()>)> amp_worker_post,
                                         std::function<void(std::function<void()>)> amp_post_io,
                                         std::function<void(std::chrono::milliseconds, std::function<void()>)> amp_post_after)
    : store_(store), contacts_(contacts), identity_(identity), relay_(relay), inbox_(inbox),
      account_keys_(account_keys), psk_store_(psk_store), group_roster_(group_roster),
      transports_(store, identity, account_keys.Signing(), psk_store), outbox_([this]() { return transports_.DirectChat(); }),
      epoch_coordinator_(store), psk_coordinator_(store, psk_store), public_lock_(store, psk_store), routes_(contacts),
      session_keys_(psk_store, account_keys.Kem()), own_cards_(identity, account_keys),
      card_service_(identity, own_cards_, account_keys, contacts),
      control_lane_(identity, own_cards_, contacts, routes_, session_keys_, transports_, outbox_) {
  redirectLogger("MeshDeliveryOrchestrator");
  receive_pipeline_ =
      std::make_unique<RelayReceivePipeline>(store_,
                                             [this](const std::string& account_id) -> std::optional<AccountCard> {
                                               auto kept = account_keys_.Kept(account_id);
                                               if (!kept || !kept->has_value()) {
                                                 return std::nullopt;
                                               }
                                               return (*kept)->newest_card;
                                             },
                                             psk_store_, LocalKeysFromIdentity(identity_), group_roster_,
                                             invite_gate);
  receive_pipeline_->SetContactTrust(&contacts_);
  // A session opener's newer card that verified it: the sender's devices are answerable from now on. Our own
  // account's (a sibling's notice) is our device list (M021).
  receive_pipeline_->SetOnSenderCard([this](const AccountCard& card) {
    auto identity = identity_.Get();
    if (identity && card.keys.account_id == identity->account_id) {
      if (auto learned = own_cards_.Learn(card, util::NowUnixMs()); !learned) {
        log().warning << "a sibling's card not taken: " << learned.error().message;
      }
      return;
    }
    if (auto kept = account_keys_.AddAccountCard(card, util::NowUnixMs(), "envelope"); !kept) {
      log().warning << "sender's account card not kept: " << kept.error().message;
    }
  });
  receive_pipeline_->SetAccountCardFloor(
      [this](const std::string& account_id) { return account_keys_.CardFloor(account_id); });
  // Our device list changed here (a device linked in, a sibling's newer list, the nickname): our other devices
  // hear, through our own mailbox (M022).
  own_cards_.SetOnChanged([this](const AccountCard& card) {
    log().info << "own account card now lists " << card.devices.size() << " device(s)";
    AppRuntime::PostWorkerNormal([this]() { NotifySiblings(); });
  });
  receive_pipeline_->SetOnRosterMember([this](const GroupMembershipCodec::MemberJoinedEntry& member) {
    if (member.card) {
      if (auto kept = account_keys_.AddCarriedCard(*member.card, util::NowUnixMs(), "group_roster",
                                                   member.member_identity);
          !kept) {
        log().warning << "roster card not kept: " << kept.error().message;
      }
    }
    if (IsRelayUserIdValue(member.relay_user_id)) {
      routes_.NoteAccountRoute(member.member_identity, member.relay_user_id);
    }
  });
  // A peer withdrew a message we hold: its bytes, summary and preview follow (the pipeline wiped the row).
  receive_pipeline_->SetOnMessageRetracted(
      [this](const std::string& thread_id, const ThreadMessage& original) { inbox_.AfterMessageDeleted(thread_id, original); });
  transports_.SetInboundHandler([this](RelayEnvelope envelope, std::string from_peer_id) {
    HandleDirectInbound(std::move(envelope), from_peer_id);
  });
  // Chat reaches an account over a live link to one of the devices its card lists.
  transports_.SetDevicesOf([this](const std::string& key) { return card_service_.DevicesOf(key); });
  outbox_.SetRelayClient(relay_);
  // On the worker that finished the send: the row is marked before the UI hears of it.
  outbox_.SetOnOutcome([this](const DeliveryOutcome& outcome) {
    if (!outcome.group_member.empty()) {
      ApplyGroupMemberOutcome(outcome);
    } else if (!outcome.copy_device.empty() && !outcome.ok) {
      // One copy of several (M021): the message stands delivered when another copy is.
      if (MarkDelivery(outcome.thread_id, outcome.message_id, false, MessageTransport::Relay, true) &&
          on_messages_changed_) {
        AppRuntime::PostUI([this]() { on_messages_changed_(); });
      }
    } else if (outcome.ok) {
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

std::optional<std::string> MeshDeliveryOrchestrator::RelayRouteOf(const std::string& account_id) const {
  return routes_.ResolveAccount(account_id);
}

std::optional<Object> MeshDeliveryOrchestrator::OwnCardJson() {
  auto card = own_cards_.Card(util::NowUnixMs());
  if (!card) {
    log().warning << "own account card unavailable: " << card.error().message;
    return std::nullopt;
  }
  return AccountCardToJson(*card);
}

namespace {

/** A system message the receiver takes nothing from but the card its envelope carries. */
ThreadMessage DeviceListNotice() {
  ThreadMessage notice;
  notice.id = util::GenerateUuid();
  notice.content_type = ChatContentType::System;
  notice.text = "Device list";
  notice.payload_json = R"({"control_type":"device_list"})";
  notice.timestamp = util::NowUnixMs();
  return notice;
}

} // namespace

void MeshDeliveryOrchestrator::NotifyContactsOfDevices(const std::vector<std::string>& accounts) {
  size_t told = 0;
  for (const std::string& account : accounts) {
    if (auto sent = control_lane_.Send(account, DeviceListNotice()); !sent) {
      log().info << "device list not sent to " << account << ": " << sent.error().message;
      continue;
    }
    ++told;
  }
  log().info << "device list sent to " << told << " of " << accounts.size() << " contact(s)";
}

void MeshDeliveryOrchestrator::NudgeSiblings() {
  // The device_list notice alone: nothing about our own devices changed, so no disclosure republish.
  if (auto card = own_cards_.Card(util::NowUnixMs()); !card || card->devices.size() < 2) {
    return;
  }
  if (auto sent = control_lane_.NotifySiblings(DeviceListNotice()); !sent) {
    log().info << "siblings not nudged: " << sent.error().message;
  }
}

void MeshDeliveryOrchestrator::MirrorToSiblings(const ThreadMessage& sent, SentCopyTarget target) {
  if (!sent_copy::Mirrored(sent)) {
    return;
  }
  AppRuntime::PostWorkerNormal([this, sent, target = std::move(target)]() {
    if (auto card = own_cards_.Card(util::NowUnixMs()); !card || card->devices.size() < 2) {
      return;  // no other device, or signed out
    }
    auto notice = sent_copy::BuildNotice(sent, target);
    if (!notice) {
      log().warning << "sent copy not built message_id=" << sent.id << ": " << notice.error().message;
      return;
    }
    if (auto copied = control_lane_.NotifySiblings(*notice); !copied) {
      log().info << "sent copy not sent message_id=" << sent.id << ": " << copied.error().message;
    }
  });
}

void MeshDeliveryOrchestrator::NotifySiblings() {
  if (auto card = own_cards_.Card(util::NowUnixMs())) {
    siblings_told_card_at_.store(card->issued_at);
  }
  if (auto sent = control_lane_.NotifySiblings(DeviceListNotice()); !sent) {
    log().info << "siblings not told of the device list: " << sent.error().message;
  }
  if (on_own_devices_changed_) {
    on_own_devices_changed_();
  }
}

bool MeshDeliveryOrchestrator::LinkedTo(const std::string& peer_id) const {
  IDirectMessageClient* chat = transports_.DirectChat();
  return chat && chat->IsPeerConnected(peer_id);
}

Roe<EnvelopeSender> MeshDeliveryOrchestrator::OwnSender(const LocalIdentity& identity) {
  if (own_cards_.SignedOut()) {
    return Error(kSignedOutMessage);  // M021 d4: the account's newest list leaves this device out
  }
  auto device = own_cards_.DevicePeerId();
  if (!device) {
    return device.error();
  }
  return EnvelopeSender{.relay_user_id = identity.relay_user_id,
                        .account_id = identity.account_id,
                        .device = *device,
                        .card = OwnCardJson()};
}

std::vector<AccountCardDevice> MeshDeliveryOrchestrator::PeerDevices(const std::string& account_id) {
  auto kept = account_keys_.Kept(account_id);
  if (!kept || !kept->has_value() || !(*kept)->newest_card) {
    return {};
  }
  return (*kept)->newest_card->devices;
}

Roe<void> MeshDeliveryOrchestrator::SignEnvelope(RelayEnvelope& envelope) {
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  if (!sign_bytes) {
    return sign_bytes.error();
  }
  auto signature = identity_.SignDeviceBytes(*sign_bytes);
  if (!signature) {
    return signature.error();
  }
  envelope.signature = *signature;
  return {};
}

void MeshDeliveryOrchestrator::RegisterPeerKeysFromDirectory(const mesh::DirectoryHit& hit) {
  if (auto kept = account_keys_.AddDirectoryHit(hit); !kept) {
    log().warning << "Directory entry's keys not kept: " << kept.error().message;
  }
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

/** What an applied membership message leaves this device to do (the pipeline decides; the workflow sends). */
void FollowUpMembershipAfterIngest(GroupMembershipWorkflow* groups, const RelayReceiveOutcome& outcome) {
  if (!groups) {
    return;
  }
  if (outcome.publish_member_joined_group_id && outcome.publish_member_joined_member_identity) {
    (void)groups->PublishMemberJoined(*outcome.publish_member_joined_group_id,
                                      *outcome.publish_member_joined_member_identity,
                                      outcome.publish_member_joined_epoch);
  }
  if (outcome.resign_roster_group_id) {
    (void)groups->SignAndSendRoster(*outcome.resign_roster_group_id);
  }
  if (outcome.refuse_invite_group_id) {
    (void)groups->RefuseInvite(*outcome.refuse_invite_group_id, outcome.refuse_invite_nonce,
                               outcome.refuse_invite_member);
  }
  if (outcome.took_ownership_group_id) {
    (void)groups->SignAndSendRoster(*outcome.took_ownership_group_id, outcome.took_ownership_transfer);
  }
  if (outcome.restart_roster_check_group_id) {
    (void)groups->StartRosterCheck(*outcome.restart_roster_check_group_id);
  }
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

void MeshDeliveryOrchestrator::HandleDirectInbound(RelayEnvelope envelope, const std::string& from_peer_id) {
  auto identity = identity_.Get();
  if (!identity) {
    return;
  }
  const RelayReceiveOutcome outcome =
      receive_pipeline_->ProcessEnvelope(envelope, identity->account_id, false, MessageTransport::Direct);
  routes_.RememberFromEnvelope(envelope, outcome);
  // A verified message over a link with a device its account's card does not list yet: ask that device
  // for the card (it proves the device is the account's; the envelope signature alone does not).
  if (outcome.sender_verified) {
    card_service_.NoteDevice(envelope.sender_contact_id, from_peer_id);
  }
  FollowUpMembershipAfterIngest(groups_, outcome);
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
    NoteMention(outcome, identity->account_id);
    // Over a link, as from the relay: the user away gets a notice (a mention alerts in a muted group, G015).
    if (auto notice = BackgroundNoticeFor(outcome, identity->account_id); notice && on_background_unread_) {
      AppRuntime::PostUI([this, notice = std::move(*notice)]() mutable {
        if (on_background_unread_) {
          on_background_unread_(std::move(notice.title), std::move(notice.body), std::move(notice.thread_id),
                                notice.mentions_us);
        }
      });
    }
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

Roe<EnsuredE2ePublicSessionKey> MeshDeliveryOrchestrator::EnsureE2ePublicSessionKey(
    const std::string& peer_identity) {
  return session_keys_.Ensure(peer_identity);
}

Roe<ByteVector> MeshDeliveryOrchestrator::PeerKemPublicKey(const ChatTargetKey& key) const {
  if (key.peer_device.empty()) {
    return session_keys_.PeerKemPublicKey(key);  // the account's (an account copy)
  }
  // The device's own, as the peer's newest kept card lists it (M021).
  auto kept = account_keys_.Kept(key.peer_identity_value);
  if (!kept || !kept->has_value() || !(*kept)->newest_card) {
    return Error("No account card for the peer");
  }
  const AccountCardDevice* device = FindCardDevice(*(*kept)->newest_card, key.peer_device);
  if (!device) {
    return Error("The peer's card does not list that device");
  }
  return Base64Decode(device->kem_public_key_b64);
}

Roe<void> MeshDeliveryOrchestrator::SendControl(const std::string& peer_identity, const ThreadMessage& control,
                                                const std::optional<std::string>& key_init_b64, const bool urgent) {
  return control_lane_.Send(peer_identity, control, key_init_b64, urgent);
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
                                            const bool success, const MessageTransport transport,
                                            const bool unless_delivered) {
  if (thread_id.empty()) {
    return false;  // a control-lane send: no transcript row to update
  }
  auto messages = store_.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!messages) {
    return false;
  }
  for (ThreadMessage& existing : *messages) {
    if (existing.id == message_id) {
      if (unless_delivered && existing.delivery == MessageDelivery::Relayed) {
        return false;
      }
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

Roe<void> MeshDeliveryOrchestrator::StartNextEpochIfRestored(const Thread& thread) {
  return StartNextPairEpochIfRestored(E2eRelayPayloadCodec::ChatTargetFromThread(thread));
}

Roe<void> MeshDeliveryOrchestrator::StartNextPairEpochIfRestored(const ChatTargetKey& key) {
  auto loaded = psk_store_.Load(key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value() || !(*loaded)->restart_pending) {
    return {};
  }
  // This device lost its numbering in the pair (its sessions kept through *Reset chats*): the pair moves to
  // its next epoch at seq 1 instead of rewinding in this one (D014, #378) — for the 1:1 chat and for every
  // group copy, which are numbered in the pair too (G010). A fresh session opens there with this send's
  // key_init, which is how the peer follows; the old one is kept retired.
  const uint32_t old_epoch = (*loaded)->session_epoch;
  uint32_t new_epoch = old_epoch + 1;
  DirectChatTarget direct{key.peer_identity_kind, key.peer_identity_value, ThreadChannel::E2ePublic};
  if (!key.peer_device.empty()) {
    // A device pair numbers on its own (M021): its session's next epoch, the thread's count untouched.
  } else if (auto thread = store_.FindDirectThread(direct); thread && thread->has_value()) {
    auto bumped = store_.BumpLocalChatTargetEpoch((**thread).id);  // the 1:1 count starts over too
    if (!bumped) {
      return bumped.error();
    }
    new_epoch = *bumped;
  }
  if (auto restarted = RestartE2ePublicSession(psk_store_, key, old_epoch, new_epoch, util::NowUnixMs()); !restarted) {
    return restarted.error();
  }
  log().info << "restored pair restarts at epoch " << new_epoch << " (was " << old_epoch
             << ") peer=" << key.peer_identity_value;
  return {};
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

  if ((*thread)->kind == ThreadKind::Direct && (*thread)->channel == ThreadChannel::E2ePublic) {
    if (auto restarted = StartNextEpochIfRestored(**thread); !restarted) {
      return restarted.error();
    }
  }
  ThreadMessage outbound = NewOutboundMessage(thread_id, text, options, util::GenerateUuid(), util::NowUnixMs());
  if (options.stored_payload_json) {
    outbound.payload_json = *options.stored_payload_json;
  }
  auto appended = AppendOutboundMessage(store_, std::move(outbound));
  if (!appended) {
    return appended.error();
  }
  // What goes on the wire: the row as numbered, with everything the send carries.
  ThreadMessage wire = *appended;
  if (options.payload_json) {
    wire.payload_json = *options.payload_json;
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
  auto sender = OwnSender(*identity);
  if (!sender) {
    return FailOutbound(store_, *appended, sender.error());
  }
  // Brief recipient is relay:; Amp-only (no route yet) uses Account as party placeholder.
  const std::string route_party = peer_relay_id ? *peer_relay_id : amp_peer_key;
  // A public chat goes to each of the peer's devices on its card, each copy in its device pair (M021); with no
  // device known yet (a first contact), or a key rotation of the account-level session, as one account copy.
  const bool rotating = options.key_init_b64 && !options.key_init_b64->empty();
  const std::vector<AccountCardDevice> devices =
      (*thread)->channel == ThreadChannel::E2ePublic && !rotating ? PeerDevices(amp_peer_key)
                                                                   : std::vector<AccountCardDevice>{};
  if (!devices.empty()) {
    // M022: a device we have a live link to gets its copy over it (the relay if that fails — a record of its
    // own); every other device's copy shares one relay record to the account.
    std::optional<Error> failure;
    bool any = false;
    std::vector<RelayCopy> relay_copies;
    for (const AccountCardDevice& device : devices) {
      const ChatTargetKey pair = GroupE2ePayloadCodec::PairTargetKey(amp_peer_key, device.peer_id);
      if (auto restarted = StartNextPairEpochIfRestored(pair); !restarted) {
        log().warning << "device pair restart failed peer=" << amp_peer_key << ": " << restarted.error().message;
      }
      auto device_kem = Base64Decode(device.kem_public_key_b64);
      auto copy = device_kem ? SealPairCopy(wire, identity->account_id, options, psk_store_, pair, *device_kem)
                             : Roe<PairCopy>(device_kem.error());
      if (!copy) {
        log().warning << "copy for device " << device.peer_id << " not sealed: " << copy.error().message;
        failure = copy.error();
        continue;
      }
      RelayCopy sealed_copy = DeviceCopy(device.peer_id, std::move(*copy));
      const bool linked = LinkedTo(device.peer_id);
      log().info << "chat copy message_id=" << appended->id << " device=" << device.peer_id
                 << (linked ? " over its link" : " by relay");
      if (!linked) {
        relay_copies.push_back(std::move(sealed_copy));
        continue;
      }
      RelayEnvelope envelope = BuildDirectEnvelope(wire, *sender, (*thread)->channel, route_party,
                                                   {std::move(sealed_copy)}, NextRelayOrderKey());
      if (auto signed_ok = SignEnvelope(envelope); !signed_ok) {
        failure = signed_ok.error();
        continue;
      }
      outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                                   .thread_id = thread_id,
                                   .message_id = appended->id,
                                   .amp_peer_key = device.peer_id,
                                   .relay_route = peer_relay_id,
                                   .critical_lane = options.critical_lane,
                                   .copy_device = device.peer_id});
      any = true;
    }
    if (!relay_copies.empty()) {
      RelayEnvelope envelope = BuildDirectEnvelope(wire, *sender, (*thread)->channel, route_party,
                                                   std::move(relay_copies), NextRelayOrderKey());
      if (auto signed_ok = SignEnvelope(envelope); !signed_ok) {
        failure = signed_ok.error();
      } else {
        // The relay; an Amp-only peer (no route), over a link to one of its devices.
        outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                                     .thread_id = thread_id,
                                     .message_id = appended->id,
                                     .amp_peer_key = peer_relay_id ? std::string() : amp_peer_key,
                                     .relay_route = peer_relay_id,
                                     .critical_lane = options.critical_lane,
                                     .copy_device = "relay"});
        any = true;
      }
    }
    if (!any) {
      return FailOutbound(store_, *appended, failure.value_or(Error("No copy could be sealed")));
    }
  } else {
    auto sealed = SealDirectPayload(wire, **thread, identity->account_id, options, psk_store_,
                                    [this](const ChatTargetKey& key) { return PeerKemPublicKey(key); });
    if (!sealed) {
      return FailOutbound(store_, *appended, sealed.error());
    }
    RelayEnvelope envelope = BuildDirectEnvelope(*appended, *sender, (*thread)->channel, route_party,
                                                 {AccountCopy(*appended, std::move(*sealed))}, NextRelayOrderKey());
    if (auto signed_ok = SignEnvelope(envelope); !signed_ok) {
      return FailOutbound(store_, *appended, signed_ok.error());
    }
    // Amp dial key is Account ID (invite listen multiaddrs / contact endpoints). Brief uses relay:.
    outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                                 .thread_id = thread_id,
                                 .message_id = appended->id,
                                 .amp_peer_key = amp_peer_key,
                                 .relay_route = peer_relay_id,
                                 .critical_lane = options.critical_lane});
  }

  // Always hop to UI — SendUserMessage runs on IO during membership fan-out (PublishMemberJoined).
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
  if (!system_control && (*thread)->channel == ThreadChannel::E2ePublic) {
    (void)public_lock_.NoteTraffic(thread_id);
    // Public chats only: a private Secure chat stays on its device (M014).
    MirrorToSiblings(wire, SentCopyTarget{.peer_account = amp_peer_key});
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
  // G013: a restored group checks its members before it sends (a stale roster would encrypt to someone
  // removed since); a group we were removed from is read-only. Refused before anything is stored.
  if (auto metadata = group_roster_.LoadMetadata(*(*thread)->group_id);
      metadata && *metadata && !group_roster_sync::MaySend(**metadata)) {
    return Error((*metadata)->sync_state == GroupSyncState::Removed ? group_roster_sync::kGroupRemovedError
                                                                        : group_roster_sync::kGroupCheckingError);
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

  // One copy per member device on the member's card, each numbered in its device pair (M021, G010/G011); a
  // member we know no device of yet gets one account copy.
  std::vector<GroupMemberTarget> targets;
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity == identity->account_id) {
      continue;
    }
    std::vector<std::string> devices;
    for (const AccountCardDevice& device : PeerDevices(member.member_identity)) {
      devices.push_back(device.peer_id);
    }
    if (devices.empty()) {
      devices.emplace_back();
    }
    for (const std::string& device : devices) {
      GroupMemberTarget target;
      target.member_identity = member.member_identity;
      target.target_key = GroupE2ePayloadCodec::PairTargetKey(member.member_identity, device);
      // A pair this device lost its count in restarts first, as a 1:1 send does.
      if (auto restarted = StartNextPairEpochIfRestored(target.target_key); !restarted) {
        log().warning << "group send: pair restart failed peer=" << member.member_identity << ": "
                      << restarted.error().message;
      }
      auto pair = psk_store_.Load(target.target_key);
      target.session_epoch = pair && pair->has_value() ? (*pair)->session_epoch : 1;
      auto seq = group_roster_.AllocateMemberSeq(*(*thread)->group_id, member.member_identity, target.session_epoch,
                                                 device);
      if (!seq) {
        return FailOutbound(store_, *appended, seq.error());
      }
      target.sender_seq = *seq;
      targets.push_back(std::move(target));
    }
  }

  auto resolve_kem = [this](const ChatTargetKey& key) { return PeerKemPublicKey(key); };

  std::optional<std::vector<uint8_t>> rich_plaintext;
  // Rich content, or a text with marked ranges (mentions, G015): the whole ChatPayload, not the bare text.
  if ((options.content_type && *options.content_type != ChatContentType::Text) ||
      !text_spans::FromPayloadJson(message.payload_json).empty()) {
    auto plaintext = ChatPayloadCodec::EncodeToRow(message);
    if (!plaintext) {
      return FailOutbound(store_, *appended, plaintext.error());
    }
    rich_plaintext = std::move(*plaintext);
  }

  const std::string group_id = *(*thread)->group_id;
  auto encrypted = GroupE2ePayloadCodec::EncryptForMembers(
      text, group_id, identity->account_id, message.id, message.timestamp, targets, psk_store_, resolve_kem,
      rich_plaintext);
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

  // Each copy goes as a direct message does: over a link to its device (or one of the member's, for an account
  // copy), else the relay under the member's `relay:` id (DeliveryOutbox). The row is marked once a copy is out.
  auto sender = OwnSender(*identity);
  if (!sender) {
    return FailOutbound(store_, *appended, sender.error());
  }
  bool any_missed = !encrypted->failed_member_identities.empty();
  // Per member (M022): copies for devices we have a live link to go over it, the rest in one relay record.
  std::map<std::string, std::vector<RelayCopy>> relay_copies;
  std::vector<std::string> members_in_order;
  for (size_t i = 0; i < targets.size() && i < encrypted->copies.size(); ++i) {
    if (!encrypted->copies[i]) {
      continue;
    }
    const GroupMemberTarget& target = targets[i];
    const std::string& recipient = target.member_identity;
    const std::string& device = target.target_key.peer_device;
    RelayCopy copy;
    copy.recipient_device = device;
    copy.session_epoch = target.session_epoch;
    copy.sender_seq = target.sender_seq;
    copy.key_init_b64 = encrypted->copies[i]->key_init_b64;
    copy.payload_b64 = encrypted->copies[i]->payload_b64;
    if (device.empty() || !LinkedTo(device)) {
      if (relay_copies.find(recipient) == relay_copies.end()) {
        members_in_order.push_back(recipient);
      }
      relay_copies[recipient].push_back(std::move(copy));
      continue;
    }
    const std::optional<std::string> relay_route = routes_.ResolveAccountWithDirectory(recipient);
    RelayEnvelope envelope = BuildGroupMemberEnvelope(message, *sender, group_id, recipient,
                                                      relay_route.value_or(recipient), {std::move(copy)},
                                                      NextRelayOrderKey());
    if (auto signed_ok = SignEnvelope(envelope); !signed_ok) {
      MarkGroupMemberMissed(group_id, recipient);
      any_missed = true;
      continue;
    }
    outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                                 .thread_id = thread_id,
                                 .message_id = message.id,
                                 .amp_peer_key = device,
                                 .relay_route = relay_route,
                                 .group_member = recipient});
  }
  for (const std::string& recipient : members_in_order) {
    const std::optional<std::string> relay_route = routes_.ResolveAccountWithDirectory(recipient);
    if (!relay_route && !AmpReaches(recipient)) {
      MarkGroupMemberMissed(group_id, recipient);
      any_missed = true;
      continue;
    }
    RelayEnvelope envelope = BuildGroupMemberEnvelope(message, *sender, group_id, recipient,
                                                      relay_route.value_or(recipient),
                                                      std::move(relay_copies[recipient]), NextRelayOrderKey());
    if (auto signed_ok = SignEnvelope(envelope); !signed_ok) {
      MarkGroupMemberMissed(group_id, recipient);
      any_missed = true;
      continue;
    }
    outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                                 .thread_id = thread_id,
                                 .message_id = message.id,
                                 .amp_peer_key = relay_route ? std::string() : recipient,
                                 .relay_route = relay_route,
                                 .group_member = recipient});
  }
  if (any_missed) {
    NotifyGroupMembersMissed();
  }
  if (on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
  MirrorToSiblings(message, SentCopyTarget{.group_id = *(*thread)->group_id});
  return *appended;
}

void MeshDeliveryOrchestrator::MarkGroupMemberMissed(const std::string& group_id, const std::string& member) {
  if (groups_) {
    groups_->MarkMemberUnreachable(group_id, member);
  }
}

void MeshDeliveryOrchestrator::NotifyGroupMembersMissed() {
  AppRuntime::PostUI([this]() {
    if (on_delivery_notice_) {
      DeliveryNotice notice;
      notice.kind = DeliveryNotice::Kind::GroupMembersMissed;
      on_delivery_notice_(notice);
    }
  });
}

void MeshDeliveryOrchestrator::ApplyGroupMemberOutcome(const DeliveryOutcome& outcome) {
  auto thread = store_.GetThread(outcome.thread_id);
  const std::optional<std::string> group_id =
      thread && *thread && (*thread)->group_id ? (*thread)->group_id : std::nullopt;
  if (!outcome.ok) {
    if (group_id) {
      MarkGroupMemberMissed(*group_id, outcome.group_member);
    }
    NotifyGroupMembersMissed();
    return;
  }
  if (groups_ && group_id) {
    groups_->ClearMemberUnreachable(*group_id, outcome.group_member);
  }
  // The first copy out marks the row; one member's failure does not unmark it.
  if (MarkDelivery(outcome.thread_id, outcome.message_id, true, outcome.transport) && on_messages_changed_) {
    AppRuntime::PostUI([this]() { on_messages_changed_(); });
  }
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
        std::vector<BackgroundNotice> background_notices;
        for (const RelayEnvelope& envelope : messages) {
          const RelayReceiveOutcome outcome = receive_pipeline_->ProcessEnvelope(envelope, local_account_id);
          routes_.RememberFromEnvelope(envelope, outcome);
          FollowUpMembershipAfterIngest(groups_, outcome);
          if (outcome.from_sibling && on_sibling_notice_) {
            on_sibling_notice_();
          }
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
          NoteMention(outcome, local_account_id);
          if (auto notice = BackgroundNoticeFor(outcome, local_account_id)) {
            background_notices.push_back(std::move(*notice));
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
                                                            std::move(notice.thread_id), notice.mentions_us);
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
  card_service_.Attach(transports_.AccountCards());
  // Once a start, with other devices on our list: they hear our card again (a notice lost to a restart). A
  // mesh that comes up again later (network change, foreground) does not repeat it for the same card (#391:
  // one notice per bring-up, each refused and retried).
  AppRuntime::PostWorkerNormal([this]() {
    if (auto card = own_cards_.Card(util::NowUnixMs()); card && card->devices.size() > 1 &&
                                                        siblings_told_card_at_.exchange(card->issued_at) !=
                                                            card->issued_at) {
      NotifySiblings();
    }
  });
}

void MeshDeliveryOrchestrator::DetachAmpTransports() {
  if (chat_sync_) {
    chat_sync_->SetPeerHistoryClient(nullptr);
  }
  card_service_.Attach(nullptr);
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
    std::function<void(std::string, std::string, std::string, bool)> callback) {
  on_background_unread_ = std::move(callback);
}

std::optional<MeshDeliveryOrchestrator::BackgroundNotice> MeshDeliveryOrchestrator::BackgroundNoticeFor(
    const RelayReceiveOutcome& outcome, const std::string& local_account) {
  // The user is away (minimized, another window focused, a phone in the background): every chat notifies, the
  // one left open too — it is not being looked at.
  if (outcome.thread_id.empty() || AppLifecycle::IsUserAttentive()) {
    return std::nullopt;
  }
  BackgroundNotice notice;
  notice.title = "New message";
  if (auto thread = store_.GetThread(outcome.thread_id); thread && *thread) {
    if (std::string label = inbox_.ResolveThreadLabel(**thread).title; !label.empty()) {
      notice.title = std::move(label);
    }
  }
  const std::string preview = outcome.persisted_message ? outcome.persisted_message->text : std::string();
  notice.body = preview.empty() ? "You have a new message" : preview;
  notice.thread_id = outcome.thread_id;
  notice.mentions_us = outcome.persisted_message &&
                       InboundMentionsUs(outcome.thread_id, *outcome.persisted_message, local_account);
  return notice;
}

void MeshDeliveryOrchestrator::NoteMention(const RelayReceiveOutcome& outcome, const std::string& local_account) {
  if (!outcome.persisted_message || outcome.thread_id.empty()) {
    return;
  }
  auto thread = store_.GetThread(outcome.thread_id);
  if (!thread || !*thread || !(*thread)->group_id) {
    return;
  }
  const ThreadMessage& message = *outcome.persisted_message;
  const auto blocked = contacts_.IsAccountBlocked(message.sender_contact_id);
  const MentionVerdict verdict =
      MentionVerdictFor(message, *(*thread)->group_id, local_account, group_roster_, blocked && *blocked);
  if (verdict == MentionVerdict::Held) {
    std::lock_guard lock(held_mentions_mutex_);
    held_mentions_.push_back(HeldMention{.group_id = *(*thread)->group_id,
                                         .thread_id = outcome.thread_id,
                                         .message = message,
                                         .local_account = local_account,
                                         .held_at_ms = util::NowUnixMs()});
    return;
  }
  if (verdict == MentionVerdict::Us && mentions_ && inbox_.ActiveThreadId() != outcome.thread_id) {
    if (auto marked = mentions_->Add(outcome.thread_id, message.id, message.timestamp); !marked) {
      log().warning << "mention not marked: " << marked.error().message;
    }
  }
}

void MeshDeliveryOrchestrator::DecideHeldMentions(const std::string& group_id) {
  std::vector<HeldMention> deciding;
  {
    std::lock_guard lock(held_mentions_mutex_);
    auto split = std::stable_partition(held_mentions_.begin(), held_mentions_.end(),
                                       [&](const HeldMention& held) { return held.group_id != group_id; });
    deciding.assign(std::make_move_iterator(split), std::make_move_iterator(held_mentions_.end()));
    held_mentions_.erase(split, held_mentions_.end());
  }
  const int64_t now_ms = util::NowUnixMs();
  std::vector<HeldMention> still_held;
  for (HeldMention& held : deciding) {
    const auto blocked = contacts_.IsAccountBlocked(held.message.sender_contact_id);
    const MentionVerdict verdict =
        MentionVerdictFor(held.message, group_id, held.local_account, group_roster_, blocked && *blocked);
    if (verdict == MentionVerdict::Held) {
      if (now_ms - held.held_at_ms < kHeldMentionMaxAgeMs) {
        still_held.push_back(std::move(held));
      }
      continue;
    }
    if (verdict != MentionVerdict::Us || now_ms - held.held_at_ms >= kHeldMentionMaxAgeMs) {
      continue;  // not ours after all, or too late to alert (G015 limit 3)
    }
    RelayReceiveOutcome decided;
    decided.persisted = true;
    decided.thread_id = held.thread_id;
    decided.persisted_message = held.message;
    NoteMention(decided, held.local_account);
    if (auto notice = BackgroundNoticeFor(decided, held.local_account); notice && on_background_unread_) {
      AppRuntime::PostUI([this, notice = std::move(*notice)]() mutable {
        if (on_background_unread_) {
          on_background_unread_(std::move(notice.title), std::move(notice.body), std::move(notice.thread_id),
                                notice.mentions_us);
        }
      });
    }
    log().info << "held mention decided group_id=" << group_id << " message_id=" << held.message.id;
  }
  if (!still_held.empty()) {
    std::lock_guard lock(held_mentions_mutex_);
    for (HeldMention& held : still_held) {
      held_mentions_.push_back(std::move(held));
    }
  }
}

bool MeshDeliveryOrchestrator::InboundMentionsUs(const std::string& thread_id, const ThreadMessage& message,
                                                 const std::string& local_account) {
  auto thread = store_.GetThread(thread_id);
  if (!thread || !*thread || !(*thread)->group_id) {
    return false;
  }
  const auto blocked = contacts_.IsAccountBlocked(message.sender_contact_id);
  return MessageMentionsUs(message, *(*thread)->group_id, local_account, group_roster_, blocked && *blocked);
}

} // namespace pbr
