#include "lib/messaging/ControlLaneEnvelope.h"

#include "common/chat/RelayStreamKey.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "lib/messaging/EnvelopeSigner.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

Roe<RelayEnvelope> BuildControlLaneEnvelope(const ControlLaneSendParams& params, const ThreadMessage& control,
                                            const std::function<Roe<std::string>(const std::vector<uint8_t>&)>& sign) {
  if (control.content_type != ChatContentType::System || control.id.empty()) {
    return Error("control lane carries identified system messages only");
  }
  if (params.sender_account_id.empty() || params.sender_device.empty() || params.peer_account_id.empty() ||
      params.route_party.empty() || !sign) {
    return Error("control lane send missing a party");
  }
  auto plaintext = ChatPayloadCodec::EncodeToRow(control);
  if (!plaintext) {
    return plaintext.error();
  }
  E2eEncryptParams encrypt;
  encrypt.text = control.text;
  encrypt.channel = CryptoChannel::E2ePublic;
  encrypt.peer_contact_id = params.peer_account_id;
  encrypt.sender_contact_id = params.sender_account_id;
  encrypt.message_id = control.id;
  encrypt.sender_seq = 0;
  encrypt.session_epoch = params.session_epoch;
  encrypt.timestamp = control.timestamp;
  auto encrypted =
      E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(encrypt, *plaintext, params.master_psk, params.key_init_b64);
  if (!encrypted) {
    return encrypted.error();
  }

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = control.id;
  envelope.sender_relay_id = params.sender_relay_id;
  envelope.sender_contact_id = params.sender_account_id;
  envelope.sender_device = params.sender_device;
  envelope.route.kind = kRelayRouteControl;
  envelope.route.channel = ThreadChannel::E2ePublic;
  RelayCopy copy;  // one account copy: every device of the peer reads the lane (M021)
  copy.session_epoch = params.session_epoch;
  copy.sender_seq = 0;
  copy.payload_b64 = std::move(encrypted->payload_b64);
  if (encrypted->key_init_b64 && !encrypted->key_init_b64->empty()) {
    copy.key_init_b64 = std::move(*encrypted->key_init_b64);
  }
  // An account copy: our card rides it, for a peer that knows none of our devices yet (M021).
  envelope.body.e2e.sender_card = params.sender_card;
  if (params.sender_card) {
    envelope.body.e2e.sender_card_at = params.sender_card->getIf<int64_t>("issued_at");
  }
  envelope = WithSingleRelayCopy(std::move(envelope), std::move(copy));
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
