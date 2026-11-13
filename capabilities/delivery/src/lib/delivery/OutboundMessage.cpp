#include "lib/delivery/OutboundMessage.h"

#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/E2ePublicSessionLogic.h"
#include "lib/delivery/E2eRelayPayloadCodec.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/delivery/RelayClients.h"
#include "lib/delivery/RelayWirePayload.h"

#include "common/chat/MessagingLimits.h"
#include "common/chat/RelayStreamKey.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/error/AppError.h"

#include <algorithm>
#include <utility>

namespace pbr {

ThreadMessage NewOutboundMessage(const std::string& thread_id, const std::string& text, const SendRelayOptions& options,
                                 std::string id, const int64_t now_ms) {
  ThreadMessage message;
  message.id = options.message_id ? *options.message_id : std::move(id);
  message.thread_id = thread_id;
  message.sender_contact_id = options.sender_contact_id.value_or(kLocalSelfContactId);
  message.text = text;
  message.timestamp = options.timestamp_ms.value_or(now_ms);
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
      fields && (fields->annotation_type == kAnnotationTypeRetract || fields->annotation_type == kAnnotationTypeEdit)) {
    return {}; // the cap guards against reaction floods; it must never stop a delete or an edit (only one is kept)
  }
  auto reached = AnnotationCapReached(store, thread_id, target_id);
  if (!reached) {
    return reached.error();
  }
  if (*reached) {
    return Error("Too many reactions on this message");
  }
  return {};
}

Roe<bool> AnnotationCapReached(const IThreadStore& store, const std::string& thread_id, const std::string& target_id) {
  auto rows = store.CountAnnotationsForTarget(thread_id, target_id); // every annotation row: cheap, no decrypt
  if (!rows) {
    return rows.error();
  }
  if (static_cast<size_t>(*rows) < kMaxAnnotationsPerTarget) {
    return false;
  }
  // As many rows as the cap: only those it is for count. A delete or an edit is exempt from it, so it does not use it up
  // either (each edit keeps a row, blanked once superseded: a message edited often still takes reactions).
  auto annotations = store.GetAnnotationsForTarget(thread_id, target_id);
  if (!annotations) {
    return annotations.error();
  }
  const auto counted = std::count_if(annotations->begin(), annotations->end(), [](const ThreadMessage& annotation) {
    auto fields = ChatPayloadCodec::DecodeAnnotationJson(annotation.payload_json);
    return !fields ||
           (fields->annotation_type != kAnnotationTypeRetract && fields->annotation_type != kAnnotationTypeEdit);
  });
  return static_cast<size_t>(counted) >= kMaxAnnotationsPerTarget;
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
                            IPeerSessionStore& psk_store, const PeerKemPublicFn& peer_kem) {
  const PeerSessionKey target = E2eRelayPayloadCodec::ChatTargetFromThread(thread);
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

namespace {

/** `message` (numbered) sealed under `master_psk` for `peer_account_id`, the key_init riding along when given. */
Roe<SealedPayload> EncryptNumbered(const ThreadMessage& message, const CryptoChannel channel,
                                   const std::string& sender_account_id, const std::string& peer_account_id,
                                   const SendRelayOptions& options, const ByteVector& master_psk,
                                   const std::optional<std::string>& key_init_b64) {
  E2eEncryptParams params;
  params.text = message.text;
  params.channel = channel;
  params.peer_contact_id = peer_account_id;
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
      return E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(params, *plaintext, master_psk, key_init_b64);
    }
    return E2eRelayPayloadCodec::EncryptTextWithAutoKey(params, master_psk, key_init_b64);
  }();
  if (!encrypted) {
    return encrypted.error();
  }
  return SealedPayload{std::move(encrypted->payload_b64), std::move(encrypted->key_init_b64)};
}

} // namespace

Roe<SealedPayload> SealDirectPayload(const ThreadMessage& message, const Thread& thread,
                                     const std::string& sender_account_id, const SendRelayOptions& options,
                                     IPeerSessionStore& psk_store, const PeerKemPublicFn& peer_kem) {
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
  return EncryptNumbered(message, E2eRelayPayloadCodec::ChannelFromThread(thread.channel), sender_account_id,
                         thread.peer_identity_value, options, key->master_psk, key->key_init_b64);
}

Roe<PairCopy> SealPairCopy(const ThreadMessage& message, const std::string& sender_account_id,
                           const SendRelayOptions& options, IPeerSessionStore& psk_store, const PeerSessionKey& pair,
                           const ByteVector& peer_device_kem) {
  if (pair.peer_device.empty() || pair.channel != CryptoChannel::E2ePublic) {
    return Error("A pair copy is for one device, on a public chat");
  }
  // The session first (a new one is made here), then our number in it.
  auto session =
      ResolveE2ePublicSendSession(psk_store, pair, [&peer_device_kem]() -> Roe<ByteVector> { return peer_device_kem; });
  if (!session) {
    return session.error();
  }
  auto seq = psk_store.AllocateSenderSeq(pair);
  if (!seq) {
    return seq.error();
  }
  PairCopy copy{.numbered = message};
  copy.numbered.session_epoch = session->epoch;
  copy.numbered.sender_seq = *seq;
  auto sealed = EncryptNumbered(copy.numbered, pair.channel, sender_account_id, pair.peer_identity_value, options,
                                session->master_psk, session->key_init_b64);
  if (!sealed) {
    return sealed.error();
  }
  copy.sealed = std::move(*sealed);
  return copy;
}

namespace {

RelayEnvelope EnvelopeFrom(const ThreadMessage& message, const EnvelopeSender& sender, std::vector<RelayCopy> copies,
                           const uint64_t order_key) {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = message.id;
  envelope.sender_relay_id = sender.relay_user_id;
  envelope.sender_contact_id = sender.account_id;
  envelope.sender_device = sender.device;
  envelope.order_key = order_key;
  envelope.timestamp = message.timestamp;
  // Our card rides a session opener, and every account copy: either may reach someone who knows none of our
  // devices yet — the card is what they check our device's signature with (M021).
  const bool introduces = std::any_of(copies.begin(), copies.end(), [](const RelayCopy& copy) {
    return (copy.key_init_b64 && !copy.key_init_b64->empty()) || copy.recipient_device.empty();
  });
  if (introduces) {
    envelope.body.e2e.sender_card = sender.card;
  }
  if (sender.card) {
    envelope.body.e2e.sender_card_at = sender.card->getIf<int64_t>("issued_at"); // M021 d6: on every envelope
  }
  if (copies.size() == 1) {
    const RelayCopy only = copies.front();
    envelope = WithSingleRelayCopy(std::move(envelope), only);
  } else {
    envelope.body.e2e.copies = std::move(copies);
  }
  return envelope;
}

} // namespace

RelayCopy AccountCopy(const ThreadMessage& numbered, SealedPayload sealed) {
  RelayCopy copy;
  copy.session_epoch = numbered.session_epoch.value_or(1);
  copy.sender_seq = numbered.sender_seq.value_or(0);
  if (sealed.key_init_b64 && !sealed.key_init_b64->empty()) {
    copy.key_init_b64 = std::move(sealed.key_init_b64);
  }
  copy.payload_b64 = std::move(sealed.payload_b64);
  return copy;
}

RelayCopy DeviceCopy(const std::string& device, PairCopy pair_copy) {
  RelayCopy copy = AccountCopy(pair_copy.numbered, std::move(pair_copy.sealed));
  copy.recipient_device = device;
  return copy;
}

RelayEnvelope BuildDirectEnvelope(const ThreadMessage& message, const EnvelopeSender& sender,
                                  const ThreadChannel channel, const std::string& route_party,
                                  std::vector<RelayCopy> copies, const uint64_t order_key) {
  RelayEnvelope envelope = EnvelopeFrom(message, sender, std::move(copies), order_key);
  envelope.route.kind = "direct";
  envelope.route.channel = channel;
  envelope.stream_key = BuildDeviceRelayStreamKey(sender.relay_user_id, route_party, channel, sender.device);
  envelope.recipient_contact_id = route_party;
  envelope.relay_wake_hint = SanitizeWakeHint(envelope.message_id);
  return envelope;
}

bool DirectSendTriesAmpFirst(const bool amp_reachable, const bool has_relay_route, const bool critical_lane) {
  return amp_reachable && !(critical_lane && has_relay_route);
}

bool ControlAlsoGoesDirect(const bool peer_connected, const bool has_relay_route, const bool critical_lane) {
  return critical_lane && has_relay_route && peer_connected;
}

RelayEnvelope BuildGroupMemberEnvelope(const ThreadMessage& message, const EnvelopeSender& sender,
                                       const std::string& group_id, const std::string& recipient,
                                       const std::string& route_party, std::vector<RelayCopy> copies,
                                       const uint64_t order_key) {
  RelayEnvelope envelope = EnvelopeFrom(message, sender, std::move(copies), order_key);
  envelope.route.kind = "group";
  envelope.stream_key = BuildGroupRelayStreamKey(group_id, sender.account_id, recipient, sender.device);
  envelope.recipient_contact_id = route_party;
  envelope.relay_wake_hint = SanitizeWakeHint(envelope.message_id);
  return envelope;
}

bool OutboxSendStillWanted(const bool control_lane, const int64_t age_ms, const bool thread_paused) {
  return control_lane ? age_ms <= kControlRetryWindowMs : !thread_paused;
}

namespace {

/** The relay refused the request itself (#391): the same bytes would be refused again. */
bool RelayRefused(const Error& failure) {
  if (IsRelayUserNotRegistered(failure)) {
    return true; // a send's 403 "Relay user not registered": a refusal as it was before it was told apart
  }
  return AppError::CategoryOf(failure) == ErrorCategory::Network &&
         (static_cast<Err::Network>(failure.code) == Err::Network::HttpRejected ||
          static_cast<Err::Network>(failure.code) == Err::Network::HttpNotFound);
}

} // namespace

bool RelayRecipientUnknown(const Error& failure) {
  return AppError::CategoryOf(failure) == ErrorCategory::Network &&
         static_cast<Err::Network>(failure.code) == Err::Network::HttpNotFound;
}

bool OutboxRetryAgain(const int attempts, const Error& failure) {
  return !RelayRefused(failure) && attempts < kMaxOutboxRetryAttempts;
}

std::optional<std::chrono::milliseconds> ControlRetryAfter(const int attempts, const int64_t age_ms,
                                                           const Error& failure) {
  if (RelayRefused(failure)) {
    return std::nullopt;
  }
  const int doublings = std::clamp(attempts - 1, 0, 4);
  const auto delay = std::min(std::chrono::milliseconds(1000 << doublings), kControlRetryMaxDelay);
  if (age_ms + delay.count() > kControlRetryWindowMs) {
    return std::nullopt;
  }
  return delay;
}

} // namespace pbr
