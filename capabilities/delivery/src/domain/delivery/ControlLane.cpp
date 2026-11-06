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

ControlLane::ControlLane(IdentityStore& identity, OwnCards& own_keys, ContactsStore& contacts, PeerRouteBook& routes,
                         E2ePublicSessionKeys& keys, std::function<IDirectMessageClient*()> direct_chat, DeliveryOutbox& outbox)
    : identity_(identity), own_keys_(own_keys), contacts_(contacts), routes_(routes), keys_(keys), direct_chat_(std::move(direct_chat)),
      outbox_(outbox) {
  redirectLogger("ControlLane");
}

ControlLane::~ControlLane() {
  lifetime_.InvalidateAndWait();
}

bool ControlLane::CarryCard(const std::string& account, const bool opens_session, const bool asked,
                            const int64_t card_at) {
  std::lock_guard lock(cards_sent_mutex_);
  auto it = cards_sent_.find(account);
  const std::optional<int64_t> told = it != cards_sent_.end() ? std::optional<int64_t>(it->second) : std::nullopt;
  if (!ControlLaneCarriesCard(opens_session, asked, told, card_at)) {
    return false;
  }
  cards_sent_[account] = card_at;
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

Roe<void> ControlLane::Send(const std::string& peer_identity, const ThreadMessage& control,
                            const std::optional<std::string>& key_init_b64, const bool urgent, const bool with_card) {
  auto identity = identity_.Get();
  if (!identity || identity->account_id.empty()) {
    return Error("Local account identity unavailable");
  }
  auto session = keys_.Resolve(peer_identity);
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
  IDirectMessageClient* chat = direct_chat_ ? direct_chat_() : nullptr;
  if (!peer_relay_id && !(chat && chat->IsPeerReachable(peer_identity))) {
    return Error("No route to the peer");
  }

  ControlLaneSendParams params;
  params.sender_account_id = identity->account_id;
  params.sender_relay_id = identity->relay_user_id;
  params.peer_account_id = peer_identity;
  params.route_party = peer_relay_id ? *peer_relay_id : peer_identity;
  params.session_epoch = session->epoch;
  params.master_psk = std::move(session->master_psk);
  auto device = own_keys_.DevicePeerId();
  if (!device) {
    return device.error();
  }
  params.sender_device = *device;
  // The card (~13 KB + 4 KB a device) only when the peer may not hold it; the version always (M021 d6).
  if (auto own = own_keys_.Card(util::NowUnixMs())) {
    params.sender_card_at = own->issued_at;
    const bool opens_session = key_init.has_value() && !key_init->empty();
    if (CarryCard(peer_identity, opens_session, with_card || urgent, own->issued_at)) {
      params.sender_card = AccountCardToJson(*own);
    }
  }
  params.key_init_b64 = std::move(key_init);
  params.order_key = NextOrderKey();
  auto built = BuildControlLaneEnvelope(
      params, control, [this](const std::vector<uint8_t>& bytes) { return identity_.SignDeviceBytes(bytes); });
  if (!built) {
    return built.error();
  }
  RelayEnvelope envelope = std::move(*built);
  envelope.relay_wake_urgent = urgent;
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
  if (CarryCard(identity->account_id, false, kind == SiblingNotice::DeviceList, card->issued_at)) {
    params.sender_card = AccountCardToJson(*card);
  }
  params.peer_account_id = identity->account_id;
  params.route_party = identity->relay_user_id;
  params.session_epoch = 1;
  params.master_psk = std::move(opened->master_psk);
  params.key_init_b64 = std::move(opened->key_init_b64);
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
