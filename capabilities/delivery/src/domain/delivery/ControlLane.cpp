#include "domain/delivery/ControlLane.h"

#include "domain/delivery/DeliveryOutbox.h"
#include "domain/delivery/E2ePublicSessionKeys.h"
#include "domain/delivery/PeerRouteBook.h"
#include "lib/delivery/ControlLaneEnvelope.h"
#include "lib/delivery/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/directory/DirectoryJson.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "common/Utilities.h"
#include "common/chat/SystemControl.h"
#include "common/runtime/AppRuntime.h"

#include <algorithm>
#include <memory>
#include <mutex>

namespace pbr {

namespace {

/** The `call_id` of a call control message (`payload_json.detail` is itself a JSON string); empty if absent. */
std::string CallIdOf(const ThreadMessage& control) {
  const auto payload = TryParseObject(control.payload_json);
  const auto detail = payload ? payload->getString("detail") : std::nullopt;
  const auto detail_json = detail ? TryParseObject(*detail) : std::nullopt;
  const auto call_id = detail_json ? detail_json->getString("call_id") : std::nullopt;
  return call_id ? *call_id : std::string{};
}

} // namespace

ControlLane::ControlLane(IdentityStore& identity, OwnCards& own_keys, AccountKeyResolver& peer_cards,
                         ContactsStore& contacts, PeerRouteBook& routes, E2ePublicSessionKeys& keys,
                         std::function<IDirectMessageClient*()> direct_chat, DeliveryOutbox& outbox)
    : identity_(identity),
      own_keys_(own_keys),
      peer_cards_(peer_cards),
      contacts_(contacts),
      routes_(routes),
      keys_(keys),
      direct_chat_(std::move(direct_chat)),
      outbox_(outbox) {
  redirectLogger("ControlLane");
}

ControlLane::~ControlLane() {
  lifetime_.InvalidateAndWait();
}

std::vector<std::string> ControlLane::CardDevicesOf(const std::string& peer_identity) {
  std::vector<std::string> ids;
  if (auto kept = peer_cards_.Kept(peer_identity); kept && kept->has_value() && (*kept)->newest_card) {
    for (const AccountCardDevice& device : (*kept)->newest_card->devices) {
      ids.push_back(device.peer_id);
    }
  }
  return ids;
}

bool ControlLane::CarryCard(const std::string& account, const std::vector<std::string>& devices,
                            const bool opens_session, const bool asked, const int64_t card_at) {
  std::lock_guard lock(cards_sent_mutex_);
  std::unordered_map<std::string, int64_t>& told = cards_sent_[account];
  std::vector<std::optional<int64_t>> told_at;
  told_at.reserve(devices.size());
  for (const std::string& device : devices) {
    auto it = told.find(device);
    told_at.push_back(it != told.end() ? std::optional<int64_t>(it->second) : std::nullopt);
  }
  if (!ControlLaneCarriesCard(opens_session, asked, told_at, card_at)) {
    return false;
  }
  for (const std::string& device : devices) {
    told[device] = card_at;
  }
  return true;
}

uint64_t ControlLane::NextOrderKey() {
  // Relay index within the lane's stream: unique and rising per sender (ms * 1000 + a counter).
  const uint64_t floor = static_cast<uint64_t>(util::NowUnixMs()) * 1000;
  uint64_t current = order_key_.load(std::memory_order_relaxed);
  uint64_t next = 0;
  do {
    next = std::max(current + 1, floor);
  } while (!order_key_.compare_exchange_weak(current, next, std::memory_order_relaxed));
  return next;
}

Roe<ControlLaneCopy> ControlLane::AccountCopyFor(const std::string& peer_identity,
                                                 const std::optional<std::string>& account_key_init) {
  auto session = keys_.Resolve(peer_identity);
  if (!session) {
    return session.error();
  }
  ControlLaneCopy copy{.session_epoch = session->epoch, .master_psk = std::move(session->master_psk)};
  // The session's own opener while unconfirmed; else one the caller holds (its media-key wrap opened the session).
  copy.key_init_b64 = std::move(session->key_init_b64);
  if (!copy.key_init_b64 && account_key_init && !account_key_init->empty()) {
    copy.key_init_b64 = account_key_init;
  }
  return copy;
}

Roe<std::vector<ControlLaneCopy>> ControlLane::CopiesFor(const std::string& peer_identity,
                                                         const std::optional<std::string>& account_key_init) {
  std::vector<AccountCardDevice> devices;
  if (auto kept = peer_cards_.Kept(peer_identity); kept && kept->has_value() && (*kept)->newest_card) {
    devices = (*kept)->newest_card->devices;
  }
  size_t own_devices = 0;
  if (auto own = own_keys_.Devices(); own) {
    own_devices = own->devices.size();
  }
  if (ControlLaneUsesAccountCopy(devices.size(), own_devices)) {
    auto copy = AccountCopyFor(peer_identity, account_key_init);
    if (!copy) {
      return copy.error();
    }
    return std::vector<ControlLaneCopy>{std::move(*copy)};
  }
  // A copy per device, each in its pair's session (the chat stream's): one device's session is not another's, so
  // whichever of them opened theirs, each reads its own (B186). The caller's account-pair opener is not theirs.
  std::vector<ControlLaneCopy> copies;
  std::optional<Error> failure;
  const auto self = own_keys_.DevicePeerId();
  for (const AccountCardDevice& device : devices) {
    if (self && device.peer_id == *self) {
      continue; // a send to our own account (a card ask to a sibling): no copy for this device
    }
    auto session = keys_.ResolveDevice(peer_identity, device);
    if (!session) {
      log().warning << "control lane: no session with device " << device.peer_id << " of " << peer_identity << ": "
                    << session.error().message;
      failure = session.error();
      continue;
    }
    copies.push_back(ControlLaneCopy{.recipient_device = device.peer_id,
                                     .session_epoch = session->epoch,
                                     .master_psk = std::move(session->master_psk),
                                     .key_init_b64 = std::move(session->key_init_b64)});
    if (copies.size() == kMaxRelayCopies) {
      break;
    }
  }
  if (copies.empty()) {
    return failure.value_or(Error("No device of the peer's to send to"));
  }
  return copies;
}

Roe<void> ControlLane::Send(const std::string& peer_identity, const ThreadMessage& control,
                            const std::optional<std::string>& key_init_b64, const bool urgent, const bool with_card) {
  auto identity = identity_.Get();
  if (!identity || identity->account_id.empty()) {
    return Error("Local account identity unavailable");
  }
  auto copies = CopiesFor(peer_identity, key_init_b64);
  if (!copies) {
    return copies.error();
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
  IDirectMessageClient* chat = direct_chat_ ? direct_chat_() : nullptr;
  if (!peer_relay_id && !(chat && chat->IsPeerReachable(peer_identity))) {
    return Error("No route to the peer");
  }

  ControlLaneSendParams params;
  params.sender_account_id = identity->account_id;
  params.sender_relay_id = identity->relay_user_id;
  params.peer_account_id = peer_identity;
  params.route_party = peer_relay_id ? *peer_relay_id : peer_identity;
  const bool opens_session = std::any_of(copies->begin(), copies->end(), [](const ControlLaneCopy& copy) {
    return copy.key_init_b64 && !copy.key_init_b64->empty();
  });
  const std::vector<std::string> addressed = ControlLaneAddressedDevices(*copies, CardDevicesOf(peer_identity));
  params.copies = std::move(*copies);
  auto device = own_keys_.DevicePeerId();
  if (!device) {
    return device.error();
  }
  params.sender_device = *device;
  // The card (~13 KB + 4 KB a device) only when a device addressed may not hold it — once per record, whichever
  // copy needed it; the version always (M021 d6).
  if (auto own = own_keys_.Card(util::NowUnixMs())) {
    params.sender_card_at = own->issued_at;
    if (CarryCard(peer_identity, addressed, opens_session, with_card || urgent, own->issued_at)) {
      params.sender_card = AccountCardToJson(*own);
    }
  }
  params.order_key = NextOrderKey();
  auto built = BuildControlLaneEnvelope(
      params, control, [this](const std::vector<uint8_t>& bytes) { return identity_.SignDeviceBytes(bytes); });
  if (!built) {
    return built.error();
  }
  RelayEnvelope envelope = std::move(*built);
  envelope.relay_wake_urgent = urgent;
  if (urgent) {
    envelope.relay_wake_hint = SanitizeWakeHint(CallIdOf(control));
  }
  // Lane messages are useless once no family would take them: the relay drops them then, not after 90 days.
  envelope.relay_ttl_s = kControlLaneRelayTtlMs / 1000;
  const std::string message_id = envelope.message_id;
  // Off the caller's thread, on the critical lane (signaling must not queue behind inbox polls); the
  // relay when a route is known, and over a live link to the peer at once as well (#431: an inbox
  // can lag tens of seconds; the receiver's control lane takes the first copy). No row to mark: a
  // relay failure is retried while fresh.
  outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                               .message_id = message_id,
                               .amp_peer_key = peer_identity,
                               .relay_route = peer_relay_id,
                               .critical_lane = true});
  return {};
}

void ControlLane::SendToMany(std::vector<ControlSend> sends, ControlSendsDone done, const WorkerLane lane) {
  struct Gather {
    std::mutex mutex;
    std::vector<ControlSendResult> results;
    size_t left = 0;
    ControlSendsDone done;
  };
  if (sends.empty()) {
    if (done) {
      done({});
    }
    return;
  }
  auto gather = std::make_shared<Gather>();
  gather->results.resize(sends.size());
  gather->left = sends.size();
  gather->done = std::move(done);
  for (size_t i = 0; i < sends.size(); ++i) {
    gather->results[i].peer_identity = sends[i].peer_identity;
    // One worker each: a cold route's directory lookup does not hold up the other peers' copies.
    lifetime_.Post([lane](std::function<void()> task) { AppRuntime::PostWorker(lane, std::move(task)); },
                   [this, gather, i, send = std::move(sends[i])]() {
                     auto queued =
                         Send(send.peer_identity, send.control, send.key_init_b64, send.urgent, send.with_card);
                     bool last = false;
                     {
                       std::lock_guard lock(gather->mutex);
                       gather->results[i].queued = std::move(queued);
                       last = --gather->left == 0;
                     }
                     if (last && gather->done) {
                       gather->done(std::move(gather->results));
                     }
                   });
  }
}

Roe<void> ControlLane::NotifySiblings(const ThreadMessage& notice, const SiblingNotice kind) {
  auto identity = identity_.Get();
  if (!identity || identity->account_id.empty() || identity->relay_user_id.empty()) {
    return Error("Local account identity unavailable");
  }
  auto device = own_keys_.DevicePeerId();
  if (!device) {
    return device.error();
  }
  auto card = own_keys_.Card(util::NowUnixMs());
  if (!card) {
    return card.error();
  }
  // A one-off opener to our own account KEM (no session kept). Siblings check it with the card of our account they
  // keep, so the card rides only a device list, or the first notice after it changed.
  auto account_kem = Base64Decode(identity->kem_public_key_b64);
  if (!account_kem) {
    return account_kem.error();
  }
  auto opened = AutoKeyEstablishment::EncapsulateForRecipient(*account_kem);
  if (!opened) {
    return opened.error();
  }
  ControlLaneSendParams params;
  params.sender_account_id = identity->account_id;
  params.sender_relay_id = identity->relay_user_id;
  params.sender_device = *device;
  params.sender_card_at = card->issued_at;
  std::vector<std::string> siblings;
  for (const AccountCardDevice& sibling : card->devices) {
    if (sibling.peer_id != *device) {
      siblings.push_back(sibling.peer_id);
    }
  }
  const std::vector<std::string> addressed = ControlLaneAddressedDevices({ControlLaneCopy{}}, siblings);
  if (CarryCard(identity->account_id, addressed, false, kind == SiblingNotice::DeviceList, card->issued_at)) {
    params.sender_card = AccountCardToJson(*card);
  }
  params.peer_account_id = identity->account_id;
  params.route_party = identity->relay_user_id;
  params.copies = {ControlLaneCopy{.session_epoch = 1,
                                   .master_psk = std::move(opened->master_psk),
                                   .key_init_b64 = std::move(opened->key_init_b64)}};
  params.order_key = NextOrderKey();
  auto built = BuildControlLaneEnvelope(
      params, notice, [this](const std::vector<uint8_t>& bytes) { return identity_.SignDeviceBytes(bytes); });
  if (!built) {
    return built.error();
  }
  // A signal is fresh only; a sent copy or the device list waits for a sibling that is off for days.
  if (kind == SiblingNotice::Signal) {
    built->relay_ttl_s = kControlLaneRelayTtlMs / 1000;
  }
  const std::string message_id = built->message_id;
  // The relay only: the account's own mailbox, read by every sibling.
  outbox_.Send(DeliveryRequest{.envelope = std::move(*built),
                               .message_id = message_id,
                               .relay_route = identity->relay_user_id,
                               .critical_lane = true});
  return {};
}

} // namespace pbr
