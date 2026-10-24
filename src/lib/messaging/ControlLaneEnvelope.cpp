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
  if (params.sender_account_id.empty() || params.peer_account_id.empty() || params.route_party.empty() || !sign) {
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
  envelope.route.kind = kRelayRouteControl;
  envelope.route.channel = ThreadChannel::E2ePublic;
  envelope.body.e2e.payload_b64 = std::move(encrypted->payload_b64);
  if (encrypted->key_init_b64 && !encrypted->key_init_b64->empty()) {
    envelope.body.e2e.key_init_b64 = std::move(*encrypted->key_init_b64);
    envelope.body.e2e.sender_keys = params.sender_keys;
  }
  envelope.sender_seq = 0;
  envelope.order_key = params.order_key;
  envelope.session_epoch = params.session_epoch;
  envelope.stream_key = BuildControlRelayStreamKey(params.sender_relay_id, params.route_party);
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
