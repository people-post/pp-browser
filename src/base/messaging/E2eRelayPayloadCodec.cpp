#include "base/messaging/E2eRelayPayloadCodec.h"

#include "base/crypto/AutoKeyEstablishment.h"
#include "base/crypto/CanonicalAad.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/EncryptedPayload.h"
#include "base/crypto/MessageCipher.h"
#include "base/crypto/SessionKeyDeriver.h"
#include "base/messaging/AutoKeyEnvelopeResolver.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ChatPayloadValidator.h"

namespace pbr {

CryptoChannel E2eRelayPayloadCodec::ChannelFromThread(const ThreadChannel channel) {
  return channel == ThreadChannel::E2ePublic ? CryptoChannel::E2ePublic : CryptoChannel::E2e;
}

ChatTargetKey E2eRelayPayloadCodec::ChatTargetFromThread(const Thread& thread) {
  ChatTargetKey key;
  key.peer_identity_kind = thread.peer_identity_kind;
  key.peer_identity_value = thread.peer_identity_value;
  key.channel = ChannelFromThread(thread.channel);
  return key;
}

bool E2eRelayPayloadCodec::RequiresEncryption(const ThreadChannel channel) {
  return channel == ThreadChannel::E2e || channel == ThreadChannel::E2ePublic;
}

namespace {

AadFields AadFromEncryptParams(const E2eEncryptParams& params) {
  AadFields fields;
  fields.channel = params.channel;
  fields.peer_contact_id = params.peer_contact_id;
  fields.message_id = params.message_id;
  fields.sender_contact_id = params.sender_contact_id;
  fields.sender_seq = params.sender_seq;
  fields.session_epoch = params.session_epoch;
  fields.timestamp = params.timestamp;
  return fields;
}

Roe<AadFields> ExpectedReceiveAad(const RelayEnvelope& envelope, const std::string& local_contact_id) {
  if (envelope.sender_seq == 0 || envelope.session_epoch == 0) {
    return Error("Envelope missing seq fields for E2E decrypt");
  }
  AadFields fields;
  fields.channel = envelope.route.channel == ThreadChannel::E2ePublic ? CryptoChannel::E2ePublic : CryptoChannel::E2e;
  fields.peer_contact_id = local_contact_id;
  fields.message_id = envelope.message_id;
  fields.sender_contact_id = envelope.sender_contact_id;
  fields.sender_seq = envelope.sender_seq;
  fields.session_epoch = envelope.session_epoch;
  fields.timestamp = envelope.timestamp;
  return fields;
}

} // namespace

Roe<std::string> E2eRelayPayloadCodec::EncryptText(const E2eEncryptParams& params, const ByteVector& master_psk) {
  if (auto valid = ChatPayloadValidator::ValidateOutboundText(params.text); !valid) {
    return valid.error();
  }
  auto plaintext = ChatPayloadCodec::EncodeText(params.text);
  if (!plaintext) {
    return plaintext.error();
  }
  return EncryptChatPayloadBytes(params, *plaintext, master_psk);
}

Roe<std::string> E2eRelayPayloadCodec::EncryptChatPayloadBytes(const E2eEncryptParams& params,
                                                               const std::vector<uint8_t>& plaintext,
                                                               const ByteVector& master_psk) {
  if (plaintext.size() > kMaxE2ePlaintextBytes) {
    return Error("ChatPayload exceeds E2E plaintext limit");
  }

  auto session_key = SessionKeyDeriver::Derive(master_psk, params.channel, params.session_epoch);
  if (!session_key) {
    return session_key.error();
  }

  const AadFields aad_fields = AadFromEncryptParams(params);
  auto aad = CanonicalAad::Build(aad_fields);
  if (!aad) {
    return aad.error();
  }

  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }

  auto encrypted = MessageCipher::Encrypt(*session_key, plaintext, *aad, *nonce);
  if (!encrypted) {
    return encrypted.error();
  }

  auto blob = EncryptedPayload::EncodeBlob(*encrypted);
  if (!blob) {
    return blob.error();
  }
  return EncryptedPayload::EncodeBase64(*blob);
}

Roe<E2eEncryptResult> E2eRelayPayloadCodec::EncryptTextWithAutoKey(const E2eEncryptParams& params,
                                                                 const ByteVector& master_psk,
                                                                 const std::optional<std::string>& key_init_b64) {
  auto payload = EncryptText(params, master_psk);
  if (!payload) {
    return payload.error();
  }
  E2eEncryptResult result;
  result.payload_b64 = std::move(*payload);
  result.key_init_b64 = key_init_b64;
  return result;
}

Roe<E2eEncryptResult> E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(
    const E2eEncryptParams& params, const std::vector<uint8_t>& plaintext, const ByteVector& master_psk,
    const std::optional<std::string>& key_init_b64) {
  auto payload = EncryptChatPayloadBytes(params, plaintext, master_psk);
  if (!payload) {
    return payload.error();
  }
  E2eEncryptResult result;
  result.payload_b64 = std::move(*payload);
  result.key_init_b64 = key_init_b64;
  return result;
}

Roe<ThreadMessage> E2eRelayPayloadCodec::DecryptEnvelope(const RelayEnvelope& envelope,
                                                         const std::string& local_contact_id,
                                                         const ChatTargetKey& target_key, IPskSessionStore& psk_store,
                                                         const std::optional<ByteVector>& local_kem_private_key) {
  if (envelope.body.e2e.payload_b64.empty()) {
    return Error("Missing E2E payload");
  }

  auto expected_aad = ExpectedReceiveAad(envelope, local_contact_id);
  if (!expected_aad) {
    return expected_aad.error();
  }

  if (envelope.route.channel == ThreadChannel::E2ePublic && local_kem_private_key.has_value()) {
    auto master_psk = ResolveOrDeriveMasterPsk(envelope, target_key, psk_store, *local_kem_private_key);
    if (!master_psk) {
      return master_psk.error();
    }
    if (master_psk->size() != kMasterPskSize) {
      return Error("Invalid master PSK size");
    }

    auto session_key = SessionKeyDeriver::Derive(*master_psk, expected_aad->channel, envelope.session_epoch);
    if (!session_key) {
      return session_key.error();
    }

    auto blob_bytes = EncryptedPayload::DecodeBase64(envelope.body.e2e.payload_b64);
    if (!blob_bytes) {
      return blob_bytes.error();
    }
    auto blob = EncryptedPayload::DecodeBlob(*blob_bytes);
    if (!blob) {
      return blob.error();
    }

    auto aad = CanonicalAad::Build(*expected_aad);
    if (!aad) {
      return aad.error();
    }

    auto plaintext = MessageCipher::Decrypt(*session_key, *blob, *aad);
    if (!plaintext) {
      return plaintext.error();
    }
    if (plaintext->size() > kMaxE2ePlaintextBytes) {
      return Error("Decrypted payload exceeds limit");
    }

    auto message = ChatPayloadValidator::DecodeValidated(*plaintext);
    if (!message) {
      return message.error();
    }
    ChatPayloadValidator::SanitizeInboundFields(*message);
    return *message;
  }

  auto master_psk_b64 = psk_store.ResolveMasterPskForEpoch(target_key, envelope.session_epoch);
  if (!master_psk_b64) {
    return master_psk_b64.error();
  }
  if (!master_psk_b64->has_value()) {
    return Error("No PSK for envelope session epoch");
  }

  auto master_psk = Base64Decode(**master_psk_b64);
  if (!master_psk) {
    return master_psk.error();
  }
  if (master_psk->size() != kMasterPskSize) {
    return Error("Invalid master PSK size");
  }

  auto session_key = SessionKeyDeriver::Derive(*master_psk, expected_aad->channel, envelope.session_epoch);
  if (!session_key) {
    return session_key.error();
  }

  auto blob_bytes = EncryptedPayload::DecodeBase64(envelope.body.e2e.payload_b64);
  if (!blob_bytes) {
    return blob_bytes.error();
  }
  auto blob = EncryptedPayload::DecodeBlob(*blob_bytes);
  if (!blob) {
    return blob.error();
  }

  auto aad = CanonicalAad::Build(*expected_aad);
  if (!aad) {
    return aad.error();
  }

  auto plaintext = MessageCipher::Decrypt(*session_key, *blob, *aad);
  if (!plaintext) {
    return plaintext.error();
  }
  if (plaintext->size() > kMaxE2ePlaintextBytes) {
    return Error("Decrypted payload exceeds limit");
  }

  auto message = ChatPayloadValidator::DecodeValidated(*plaintext);
  if (!message) {
    return message.error();
  }
  ChatPayloadValidator::SanitizeInboundFields(*message);
  return *message;
}

} // namespace pbr
