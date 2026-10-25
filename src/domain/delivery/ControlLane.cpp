#include "domain/delivery/ControlLane.h"

#include "domain/delivery/DeliveryOutbox.h"
#include "domain/delivery/E2ePublicSessionKeys.h"
#include "domain/delivery/PeerRouteBook.h"
#include "domain/delivery/PeerTransports.h"
#include "lib/messaging/ControlLaneEnvelope.h"
#include "common/directory/DirectoryJson.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "common/Utilities.h"

#include <algorithm>

namespace pbr {

ControlLane::ControlLane(IdentityStore& identity, OwnCards& own_keys, ContactsStore& contacts, PeerRouteBook& routes,
                         E2ePublicSessionKeys& keys, PeerTransports& transports, DeliveryOutbox& outbox)
    : identity_(identity), own_keys_(own_keys), contacts_(contacts), routes_(routes), keys_(keys), transports_(transports),
      outbox_(outbox) {
  redirectLogger("ControlLane");
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
                            const std::optional<std::string>& key_init_b64, const bool urgent) {
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
  IDirectMessageClient* chat = transports_.DirectChat();
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
  if (key_init) {
    if (auto own = own_keys_.Card(util::NowUnixMs())) {
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
  const std::string message_id = envelope.message_id;
  // Off the caller's thread, on the critical lane (signaling must not queue behind inbox polls); the
  // relay first when a route is known (Amp's open + ack per message races media setup). No row to
  // mark: a relay failure is retried while fresh.
  outbox_.Send(DeliveryRequest{.envelope = std::move(envelope),
                               .message_id = message_id,
                               .amp_peer_key = peer_identity,
                               .relay_route = peer_relay_id,
                               .critical_lane = true});
  return {};
}

} // namespace pbr
