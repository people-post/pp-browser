#include "lib/messaging/OutboundMessage.h"

#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/E2ePublicSessionLogic.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "lib/messaging/ReactionTypes.h"
#include "lib/messaging/RelayWirePayload.h"

#include "common/chat/MessagingLimits.h"
#include "common/chat/RelayStreamKey.h"
#include "foundation/crypto/CryptoUtil.h"

#include <utility>

namespace pbr {

ThreadMessage NewOutboundMessage(const std::string& thread_id, const std::string& text,
                                 const SendRelayOptions& options, std::string id, const int64_t now_ms) {
  ThreadMessage message;
  message.id = std::move(id);
  message.thread_id = thread_id;
  message.sender_contact_id = options.sender_contact_id.value_or(kLocalSelfContactId);
  message.text = text;
  message.timestamp = now_ms;
  message.delivery = MessageDelivery::Pending;
  message.relay_visible = true;
  message.transport = MessageTransport::Relay;
  message.generation = options.generation;
  message.ai_invoke_mode = options.ai_invoke_mode;
  message.seq_owner_contact_id = options.seq_owner_contact_id;
  if (options.content_type) {
    message.content_type = *options.content_type;
  }
  if (options.payload_json) {
    message.payload_json = *options.payload_json;
  }
  if (message.content_type == ChatContentType::Annotation) {
    if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json);
        fields && !fields->target_message_id.empty()) {
      message.target_message_id = fields->target_message_id;
    }
  }
  return message;
}

Roe<void> CheckAnnotationCap(const IThreadStore& store, const std::string& thread_id, const ThreadMessage& message) {
  if (message.content_type != ChatContentType::Annotation) {
    return {};
  }
  const std::string target_id = message.target_message_id.value_or("");
  if (target_id.empty()) {
    return Error("Annotation missing target_message_id");
  }
  if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json);
      fields && fields->annotation_type == kAnnotationTypeRetract) {
    return {};  // the cap guards against reaction floods; it must never stop a delete
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

Roe<ThreadMessage> AppendOutboundMessage(IThreadStore& store, ThreadMessage message) {
  auto sender_seq = store.AllocateSenderSeq(message.thread_id);
  if (!sender_seq) {
    return sender_seq.error();
  }
  auto session_epoch = store.GetChatTargetSessionEpoch(message.thread_id);
  if (!session_epoch) {
    return session_epoch.error();
  }
  message.sender_seq = *sender_seq;
  message.session_epoch = *session_epoch;
  if (auto cap = CheckAnnotationCap(store, message.thread_id, message); !cap) {
    return cap.error();
  }
  return store.AppendMessage(message);
}

Roe<void> CheckDirectSendAllowed(const DirectSendGate& gate) {
  if (!gate.direct_thread) {
    return Error("Not a direct thread");
  }
  if (gate.channel == ThreadChannel::E2e && gate.compromised) {
    return Error("Messaging paused — rotate the encryption key or pause only");
  }
  if (gate.channel == ThreadChannel::E2e && !gate.psk_ready) {
    return Error("Verify the encryption key with your contact before sending");
  }
  if (gate.channel == ThreadChannel::E2ePublic && gate.locked_out) {
    return Error("This chat continues on another device");
  }
  if (!gate.peer_known) {
    return Error("Direct thread missing peer identity");
  }
  return {};
}

namespace {

struct SendKey {
  ByteVector master_psk;
  std::optional<std::string> key_init_b64;
};

Roe<SendKey> ResolveSendKey(const ThreadMessage& message, const Thread& thread, const SendRelayOptions& options,
                            IPskSessionStore& psk_store, const PeerKemPublicFn& peer_kem) {
  const ChatTargetKey target = E2eRelayPayloadCodec::ChatTargetFromThread(thread);
  const uint32_t epoch = *message.session_epoch;
  const bool rotating = options.key_init_b64 && !options.key_init_b64->empty();
  auto stored = psk_store.ResolveMasterPskForEpoch(target, epoch);
  if (!stored) {
    return stored.error();
  }
  if (!stored->has_value()) {
    if (rotating) {
      return Error("psk_rotate requires the current encryption key");
    }
    if (thread.channel != ThreadChannel::E2ePublic) {
      return Error("PSK not configured for this chat");
    }
    auto peer_public = peer_kem(target);
    if (!peer_public) {
      return peer_public.error();
    }
    // The session opened here is ours until the peer is heard under it: every send carries its
    // key_init (E2ePublicSessionLogic).
    auto ensured = EnsureE2ePublicMasterPsk(psk_store, target, epoch, *peer_public);
    if (!ensured) {
      return ensured.error();
    }
    return SendKey{std::move(ensured->master_psk), std::move(ensured->key_init_b64)};
  }
  auto decoded = Base64Decode(**stored);
  if (!decoded) {
    return decoded.error();
  }
  SendKey key{std::move(*decoded), std::nullopt};
  if (rotating) {
    key.key_init_b64 = options.key_init_b64;
  } else if (thread.channel == ThreadChannel::E2ePublic) {
    // A session we opened that the peer has not been heard under yet: keep announcing it.
    if (auto record = psk_store.Load(target); record && record->has_value() && (*record)->session_epoch == epoch) {
      key.key_init_b64 = (*record)->auto_key_init_pending;
    }
  }
  return key;
}

} // namespace

Roe<SealedPayload> SealDirectPayload(const ThreadMessage& message, const Thread& thread,
                                     const std::string& sender_account_id, const SendRelayOptions& options,
                                     IPskSessionStore& psk_store, const PeerKemPublicFn& peer_kem) {
  if (!E2eRelayPayloadCodec::RequiresEncryption(thread.channel)) {
    auto plaintext = RelayWirePayload::EncodePlaintextText(message.text);
    if (!plaintext) {
      return plaintext.error();
    }
    return SealedPayload{std::move(*plaintext), std::nullopt};
  }
  if (!message.sender_seq || !message.session_epoch) {
    return Error("Outbound message not numbered");
  }
  auto key = ResolveSendKey(message, thread, options, psk_store, peer_kem);
  if (!key) {
    return key.error();
  }
  E2eEncryptParams params;
  params.text = message.text;
  params.channel = E2eRelayPayloadCodec::ChannelFromThread(thread.channel);
  params.peer_contact_id = thread.peer_identity_value;
  params.sender_contact_id = sender_account_id;
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
      return E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(params, *plaintext, key->master_psk,
                                                                 key->key_init_b64);
    }
    return E2eRelayPayloadCodec::EncryptTextWithAutoKey(params, key->master_psk, key->key_init_b64);
  }();
  if (!encrypted) {
    return encrypted.error();
  }
  return SealedPayload{std::move(encrypted->payload_b64), std::move(encrypted->key_init_b64)};
}

namespace {

RelayEnvelope EnvelopeFrom(const ThreadMessage& message, const EnvelopeSender& sender) {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = message.id;
  envelope.sender_relay_id = sender.relay_user_id;
  envelope.sender_contact_id = sender.account_id;
  envelope.sender_seq = message.sender_seq.value_or(0);
  envelope.order_key = envelope.sender_seq;
  envelope.session_epoch = message.session_epoch.value_or(1);
  envelope.timestamp = message.timestamp;
  return envelope;
}

} // namespace

RelayEnvelope BuildDirectEnvelope(const ThreadMessage& message, const EnvelopeSender& sender,
                                  const ThreadChannel channel, const std::string& route_party, SealedPayload sealed) {
  RelayEnvelope envelope = EnvelopeFrom(message, sender);
  envelope.route.kind = "direct";
  envelope.route.channel = channel;
  envelope.body.e2e.payload_b64 = std::move(sealed.payload_b64);
  if (sealed.key_init_b64 && !sealed.key_init_b64->empty()) {
    envelope.body.e2e.key_init_b64 = std::move(*sealed.key_init_b64);
    envelope.body.e2e.sender_keys = sender.keys;
  }
  envelope.stream_key =
      BuildCanonicalRelayStreamKey(sender.relay_user_id, route_party, channel, envelope.session_epoch);
  envelope.recipient_contact_id = route_party;
  return envelope;
}

bool DirectSendTriesAmpFirst(const bool amp_reachable, const bool has_relay_route, const bool critical_lane) {
  return amp_reachable && !(critical_lane && has_relay_route);
}

RelayEnvelope BuildGroupMemberEnvelope(const ThreadMessage& message, const EnvelopeSender& sender,
                                       const std::string& group_id, const std::string& recipient,
                                       const std::string& route_party, std::string payload_b64,
                                       const uint32_t session_epoch, const uint64_t sender_seq,
                                       std::optional<std::string> key_init_b64) {
  RelayEnvelope envelope = EnvelopeFrom(message, sender);
  envelope.route.kind = "group";
  envelope.route.group_id = group_id;
  // Numbered in the pair, as the member sees it (G010/G011): the pair's epoch and our seq for the group.
  envelope.session_epoch = session_epoch;
  envelope.sender_seq = sender_seq;
  envelope.order_key = sender_seq;
  envelope.body.e2e.member_payloads = std::map<std::string, std::string>{{recipient, std::move(payload_b64)}};
  // A member with no pairwise session yet derives it from the key_init.
  if (key_init_b64) {
    envelope.body.e2e.key_init_b64 = std::move(*key_init_b64);
    envelope.body.e2e.sender_keys = sender.keys;
  }
  envelope.stream_key = BuildGroupRelayStreamKey(group_id, envelope.session_epoch, recipient);
  envelope.recipient_contact_id = route_party;
  return envelope;
}

bool OutboxSendStillWanted(const bool control_lane, const int64_t age_ms, const bool thread_paused) {
  return control_lane ? age_ms <= kControlRetryWindowMs : !thread_paused;
}

bool OutboxRetryAgain(const int attempts) {
  return attempts < kMaxOutboxRetryAttempts;
}

} // namespace pbr
