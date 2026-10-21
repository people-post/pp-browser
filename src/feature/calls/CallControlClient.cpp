#include "feature/calls/CallControlClient.h"
#include "domain/calls/CallSessionLogic.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/directory/DirectoryJson.h"
#include "domain/calls/CallMediaKeyStore.h"
#include "domain/messaging/PairwiseFanoutLogic.h"
#include "domain/messaging/SendRelayOptions.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SessionKeyDeriver.h"

#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

CallControlClient::CallControlClient(IThreadStore& store, IdentityStore& identity, CallSessionStore& sessions,
                                     IPskSessionStore& psk_store, const CallDeliveryPorts& delivery)
    : store_(store), identity_(identity), sessions_(sessions), psk_store_(psk_store), delivery_(delivery) {
  redirectLogger("CallControlClient");
}

Roe<std::string> CallControlClient::LocalRelayIdentity() const {
  auto identity = identity_.Get();
  if (!identity || identity->account_id.empty()) {
    return Error("Local account identity unavailable");
  }
  return identity->account_id;
}

Roe<void> CallControlClient::SendDirect(const std::string& peer_identity, const CallControlType type,
                                        const std::string& detail_json, const std::string& display) {
  if (!delivery_.send_control) {
    return Error("Call delivery not bound");
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto control = CallControlCodec::BuildSystemMessage("", type, display, detail_json, *local);
  if (!control) {
    return control.error();
  }
  std::optional<std::string> key_init;
  {
    std::lock_guard<std::mutex> lock(pending_key_init_mutex_);
    if (auto it = pending_key_init_.find(peer_identity); it != pending_key_init_.end()) {
      key_init = it->second;
    }
  }
  if (auto sent = delivery_.send_control(peer_identity, *control, key_init); !sent) {
    return sent.error();
  }
  if (key_init) {
    std::lock_guard<std::mutex> lock(pending_key_init_mutex_);
    pending_key_init_.erase(peer_identity);
  }
  return {};
}

Roe<void> CallControlClient::SendSummary(const std::string& thread_id, const CallSummaryDetail& summary,
                                         const bool local_only) {
  auto detail = CallControlCodec::EncodeSummary(summary);
  if (!detail) {
    return detail.error();
  }
  // The transcript shows the family's line; the text stands in where no line is drawn (previews).
  const std::string text = summary.media_mode == CallMediaMode::Video ? "Video call" : "Voice call";
  if (delivery_.send_to_thread && !local_only) {
    SendRelayOptions opts;
    opts.content_type = ChatContentType::System;
    Object payload;
    payload.set("control_type", CallControlTypeToWire(CallControlType::CallSummary));
    payload.set("detail", *detail);
    opts.payload_json = DumpJson(payload);
    opts.generation = "system";
    if (auto sent = delivery_.send_to_thread(thread_id, text, opts); sent) {
      return {};
    } else {
      log().info << "call summary kept local thread=" << thread_id << " (" << sent.error().message << ")";
    }
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto message = CallControlCodec::BuildSystemMessage(thread_id, CallControlType::CallSummary, text, *detail, *local);
  if (!message) {
    return message.error();
  }
  message->sender_contact_id = kLocalSelfContactId;
  if (auto appended = store_.AppendMessage(*message); !appended) {
    return appended.error();
  }
  return {};
}

Roe<void> CallControlClient::FanOutToJoined(const std::string& call_id, const CallControlType type,
                                             const std::string& detail_json, const std::string& display,
                                             const std::string& skip_identity) {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  const auto targets = SelectCallFanoutIdentities(*participants, skip_identity, true, false);
  // Best-effort: one peer failure must not block CallSfuAttach / roster to the rest.
  const auto result = FanOutPairwise(targets, PairwiseFanoutMode::BestEffort,
                                     [&](const std::string& identity) {
                                       return SendDirect(identity, type, detail_json, display);
                                     });
  for (size_t i = 0; i < result.failed_identities.size(); ++i) {
    log().warning << "FanOutToJoined send failed peer=" << result.failed_identities[i] << " type="
                  << CallControlTypeToWire(type) << " err="
                  << (i < result.failure_messages.size() ? result.failure_messages[i] : std::string{});
  }
  if (result.succeeded > 0) {
    log().info << "FanOutToJoined queued n=" << result.succeeded << " type=" << CallControlTypeToWire(type);
  }
  return {};
}

Roe<void> CallControlClient::FanOutToJoinedAndRinging(const std::string& call_id, const CallControlType type,
                                                       const std::string& detail_json, const std::string& display,
                                                       const std::string& skip_identity) {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  const auto targets = SelectCallFanoutIdentities(*participants, skip_identity, true, true);
  const auto result = FanOutPairwise(targets, PairwiseFanoutMode::BestEffort,
                                     [&](const std::string& identity) {
                                       return SendDirect(identity, type, detail_json, display);
                                     });
  for (size_t i = 0; i < result.failed_identities.size(); ++i) {
    log().warning << "FanOutToJoinedAndRinging send failed peer=" << result.failed_identities[i] << " type="
                  << CallControlTypeToWire(type) << " err="
                  << (i < result.failure_messages.size() ? result.failure_messages[i] : std::string{});
  }
  return {};
}

Roe<ByteVector> CallControlClient::ResolvePeerSessionKey(const std::string& peer_identity) const {
  ChatTargetKey target_key;
  target_key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  target_key.peer_identity_value = peer_identity;
  target_key.channel = CryptoChannel::E2ePublic;

  auto record = psk_store_.Load(target_key);
  if (!record) {
    return record.error();
  }
  if (!record->has_value()) {
    if (delivery_.ensure_peer_session_key) {
      auto ensured = delivery_.ensure_peer_session_key(peer_identity);
      if (!ensured) {
        return ensured.error();
      }
      if (ensured->first_message_key_init_b64 && !ensured->first_message_key_init_b64->empty()) {
        std::lock_guard<std::mutex> lock(pending_key_init_mutex_);
        pending_key_init_[peer_identity] = *ensured->first_message_key_init_b64;
      }
      return ensured->session_key;
    }
    return Error("No PSK session for peer");
  }
  const uint32_t active_epoch = (*record)->session_epoch;
  auto master_psk_b64 = psk_store_.ResolveMasterPskForEpoch(target_key, active_epoch);
  if (!master_psk_b64) {
    return master_psk_b64.error();
  }
  if (!master_psk_b64->has_value()) {
    if (delivery_.ensure_peer_session_key) {
      auto ensured = delivery_.ensure_peer_session_key(peer_identity);
      if (!ensured) {
        return ensured.error();
      }
      if (ensured->first_message_key_init_b64 && !ensured->first_message_key_init_b64->empty()) {
        std::lock_guard<std::mutex> lock(pending_key_init_mutex_);
        pending_key_init_[peer_identity] = *ensured->first_message_key_init_b64;
      }
      return ensured->session_key;
    }
    return Error("No PSK for active session epoch");
  }
  auto master_psk = Base64Decode(**master_psk_b64);
  if (!master_psk) {
    return master_psk.error();
  }
  return SessionKeyDeriver::Derive(*master_psk, CryptoChannel::E2ePublic, active_epoch);
}

Roe<void> CallControlClient::SendMediaKey(const std::string& call_id, const std::string& peer_identity,
                                                 const uint32_t media_epoch, const std::string& media_key_id,
                                                 const ByteVector& key_bytes) {
  auto session_key = ResolvePeerSessionKey(peer_identity);
  if (!session_key) {
    return session_key.error();
  }
  auto wrapped = CallMediaKeyStore::WrapKeyB64(*session_key, key_bytes, call_id, media_epoch, media_key_id);
  if (!wrapped) {
    return wrapped.error();
  }
  CallMediaKeyDetail key_detail;
  key_detail.call_id = call_id;
  key_detail.media_epoch = media_epoch;
  key_detail.media_key_id = media_key_id;
  key_detail.wrapped_key_b64 = *wrapped;
  auto key_json = CallControlCodec::EncodeMediaKey(key_detail);
  if (!key_json) {
    return key_json.error();
  }
  return SendDirect(peer_identity, CallControlType::CallMediaKey, *key_json, "Call media key");
}

} // namespace pbr
