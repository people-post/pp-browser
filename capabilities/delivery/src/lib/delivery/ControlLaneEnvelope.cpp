#include "lib/delivery/ControlLaneEnvelope.h"

#include "common/chat/RelayStreamKey.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/E2eRelayPayloadCodec.h"
#include "lib/delivery/EnvelopeSigner.h"

#include <algorithm>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

std::vector<std::string> ControlLaneAddressedDevices(const std::vector<ControlLaneCopy>& copies,
                                                     const std::vector<std::string>& card_devices) {
  std::vector<std::string> devices;
  for (const ControlLaneCopy& copy : copies) {
    if (!copy.recipient_device.empty()) {
      devices.push_back(copy.recipient_device);
    } else if (!card_devices.empty()) {
      devices.insert(devices.end(), card_devices.begin(), card_devices.end()); // every device reads the account copy
    } else {
      devices.emplace_back(); // a first contact: the account, its devices unknown
    }
  }
  return devices;
}

bool ControlLaneCarriesCard(const bool opens_session, const bool asked,
                            const std::vector<std::optional<int64_t>>& told_at, const int64_t card_at) {
  if (opens_session || asked || told_at.empty()) {
    return true;
  }
  return std::any_of(told_at.begin(), told_at.end(),
                     [card_at](const std::optional<int64_t>& told) { return !told || *told != card_at; });
}

bool ControlLaneUsesAccountCopy(const size_t peer_devices, const size_t own_devices) {
  if (peer_devices == 0) {
    return true; // no card for the peer: its account KEM opens a first contact (M021 §5)
  }
  return kControlLaneAccountCopyBetweenSingleDevices && peer_devices == 1 && own_devices == 1;
}

namespace {

/** `plaintext` sealed in `copy`'s session (seq 0: the lane is unsequenced), as the copy's wire entry. */
Roe<RelayCopy> SealLaneCopy(const ControlLaneSendParams& params, const ThreadMessage& control,
                            const std::vector<uint8_t>& plaintext, const ControlLaneCopy& copy) {
  E2eEncryptParams encrypt;
  encrypt.text = control.text;
  encrypt.channel = CryptoChannel::E2ePublic;
  encrypt.peer_contact_id = params.peer_account_id;
  encrypt.sender_contact_id = params.sender_account_id;
  encrypt.message_id = control.id;
  encrypt.sender_seq = 0;
  encrypt.session_epoch = copy.session_epoch;
  encrypt.timestamp = control.timestamp;
  auto encrypted =
      E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(encrypt, plaintext, copy.master_psk, copy.key_init_b64);
  if (!encrypted) {
    return encrypted.error();
  }
  RelayCopy out;
  out.recipient_device = copy.recipient_device;
  out.session_epoch = copy.session_epoch;
  out.sender_seq = 0;
  out.payload_b64 = std::move(encrypted->payload_b64);
  if (encrypted->key_init_b64 && !encrypted->key_init_b64->empty()) {
    out.key_init_b64 = std::move(*encrypted->key_init_b64);
  }
  return out;
}

} // namespace

Roe<RelayEnvelope> BuildControlLaneEnvelope(const ControlLaneSendParams& params, const ThreadMessage& control,
                                            const std::function<Roe<std::string>(const std::vector<uint8_t>&)>& sign) {
  if (control.content_type != ChatContentType::System || control.id.empty()) {
    return Error("control lane carries identified system messages only");
  }
  if (params.sender_account_id.empty() || params.sender_device.empty() || params.peer_account_id.empty() ||
      params.route_party.empty() || !sign) {
    return Error("control lane send missing a party");
  }
  if (params.copies.empty() || params.copies.size() > kMaxRelayCopies) {
    return Error("control lane send carries 1 to 8 copies");
  }
  auto plaintext = ChatPayloadCodec::EncodeToRow(control);
  if (!plaintext) {
    return plaintext.error();
  }
  std::vector<RelayCopy> copies;
  copies.reserve(params.copies.size());
  for (const ControlLaneCopy& copy : params.copies) {
    auto sealed = SealLaneCopy(params, control, *plaintext, copy);
    if (!sealed) {
      return sealed.error();
    }
    copies.push_back(std::move(*sealed));
  }

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = control.id;
  envelope.sender_relay_id = params.sender_relay_id;
  envelope.sender_contact_id = params.sender_account_id;
  envelope.sender_device = params.sender_device;
  envelope.route.kind = kRelayRouteControl;
  envelope.route.channel = ThreadChannel::E2ePublic;
  // Our card rides when the peer may not hold it (ControlLaneCarriesCard); the version rides every envelope, for a
  // peer with an older one to ask (M021 d6).
  envelope.body.e2e.sender_card = params.sender_card;
  envelope.body.e2e.sender_card_at = params.sender_card_at;
  if (params.sender_card && !envelope.body.e2e.sender_card_at) {
    envelope.body.e2e.sender_card_at = params.sender_card->getIf<int64_t>("issued_at");
  }
  if (copies.size() == 1) {
    envelope = WithSingleRelayCopy(std::move(envelope), std::move(copies.front()));
  } else {
    envelope.body.e2e.copies = std::move(copies);
  }
  envelope.order_key = params.order_key;
  // Per sending device: an account's devices share its `relay:` id, and the relay keeps one row per sender,
  // stream and index (M022).
  envelope.stream_key = BuildControlRelayStreamKey(params.sender_relay_id, params.route_party) + ":from:" +
                        params.sender_device;
  envelope.recipient_contact_id = params.route_party;
  envelope.timestamp = control.timestamp;
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  if (!sign_bytes) {
    return sign_bytes.error();
  }
  auto signature = sign(*sign_bytes);
  if (!signature) {
    return signature.error();
  }
  envelope.signature = *signature;
  return envelope;
}

} // namespace pbr
