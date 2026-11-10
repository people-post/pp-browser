#include "lib/calls/CallMediaKeyExchange.h"

#include "lib/calls/CallControlCodec.h"
#include "foundation/crypto/CryptoUtil.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

CallMediaKeyExchange::CallMediaKeyExchange(const ICallSessionReader& sessions, ICallMediaKeyStore& keys,
                                           Transport transport)
    : sessions_(sessions), keys_(keys), transport_(std::move(transport)) {
  redirectLogger("CallMediaKeyExchange");
}

Roe<CallMediaKeyExchange::EpochKey> CallMediaKeyExchange::Mint(const std::string& call_id, const uint32_t epoch) {
  auto key = keys_.GenerateEpochKey();
  if (!key) {
    return key.error();
  }
  auto key_id = keys_.PutEpochKey(call_id, epoch, *key);
  if (!key_id) {
    return key_id.error();
  }
  return EpochKey{epoch, std::move(*key_id), std::move(*key)};
}

Roe<CallMediaKeyExchange::EpochKey> CallMediaKeyExchange::Rotate(const std::string& call_id) {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  return Mint(call_id, (*session)->media_epoch + 1);
}

CallMediaKeyExchange::EpochStep CallMediaKeyExchange::StepFor(const uint32_t current_epoch, const uint32_t key_epoch) {
  if (key_epoch > current_epoch) {
    return EpochStep::Advance;
  }
  return key_epoch == current_epoch ? EpochStep::Reinstall : EpochStep::Ignore;
}

Roe<CallMediaKeyExchange::EpochKey> CallMediaKeyExchange::Current(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  const uint32_t epoch = (*session)->media_epoch;
  auto key = keys_.LoadEpochKey(call_id, epoch);
  if (!key) {
    return key.error();
  }
  if (!key->has_value()) {
    return Error("Call media key missing locally epoch=" + std::to_string(epoch));
  }
  return EpochKey{epoch, (*session)->media_key_id, std::move(**key)};
}

std::string CallMediaKeyExchange::WrapForPeer(const std::string& call_id, const EpochKey& key,
                                              const std::string& peer_identity) {
  if (!kMediaKeyAccountWrapBridge) {
    return {};
  }
  auto session_key = transport_.resolve_peer_session_key(peer_identity);
  if (!session_key) {
    log().warning << "Media key wrap skip; no peer session key call_id=" << call_id << " peer=" << peer_identity
                  << " err=" << session_key.error().message;
    return {};
  }
  auto wrapped = ICallMediaKeyStore::WrapKeyB64(*session_key, key.key, call_id, key.epoch, key.key_id);
  if (!wrapped) {
    log().warning << "Media key wrap failed call_id=" << call_id << " err=" << wrapped.error().message;
    return {};
  }
  return std::move(*wrapped);
}

Roe<void> CallMediaKeyExchange::Send(const std::string& call_id, const std::string& peer_identity,
                                     const EpochKey& key) {
  if (auto sent = transport_.send_media_key(call_id, peer_identity, key.epoch, key.key_id, key.key); !sent) {
    log().warning << "CallMediaKey send failed call_id=" << call_id << " peer=" << peer_identity
                  << " err=" << sent.error().message;
    return sent.error();
  }
  log().info << "CallMediaKey sent call_id=" << call_id << " peer=" << peer_identity << " epoch=" << key.epoch;
  return {};
}

Roe<void> CallMediaKeyExchange::SendCurrent(const std::string& call_id, const std::string& peer_identity) {
  auto key = Current(call_id);
  if (!key) {
    log().warning << "CallMediaKey not sent call_id=" << call_id << " peer=" << peer_identity
                  << " err=" << key.error().message;
    return key.error();
  }
  return Send(call_id, peer_identity, *key);
}

std::optional<ByteVector> CallMediaKeyExchange::OpenKey(const std::string& call_id, const uint32_t epoch,
                                                        const std::string& key_id, const std::string& media_key_b64,
                                                        const std::string& wrapped_key_b64,
                                                        const std::string& sender_identity, const char* what) {
  if (!media_key_b64.empty()) {
    // M024: the lane copy that carried it was sealed for this device; no session of ours to unwrap it under.
    auto key = Base64Decode(media_key_b64);
    if (key && key->size() == kCallMediaKeyBytes) {
      return std::move(*key);
    }
    log().warning << what << " media key malformed from=" << sender_identity;
    return std::nullopt;
  }
  if (!kMediaKeyAccountWrapRead || wrapped_key_b64.empty()) {
    return std::nullopt;
  }
  // The old form (an app before M024): wrapped under the account pair's session — the one held (the account copy it
  // came in was read under it). None held: a session opened now could not open it, so none is.
  auto session_key = transport_.held_peer_session_key ? transport_.held_peer_session_key(sender_identity)
                                                      : Roe<ByteVector>(Error("no held session port"));
  if (!session_key) {
    log().warning << what << " media key missing peer session key from=" << sender_identity;
    return std::nullopt;
  }
  auto unwrapped = ICallMediaKeyStore::UnwrapKeyB64(*session_key, wrapped_key_b64, call_id, epoch, key_id);
  if (!unwrapped) {
    log().warning << what << " media key unwrap failed: " << unwrapped.error().message;
    return std::nullopt;
  }
  return std::move(*unwrapped);
}

bool CallMediaKeyExchange::TakeKey(const std::string& call_id, const uint32_t epoch, const std::string& key_id,
                                   const std::string& media_key_b64, const std::string& wrapped_key_b64,
                                   const std::string& sender_identity, const char* what) {
  auto key = OpenKey(call_id, epoch, key_id, media_key_b64, wrapped_key_b64, sender_identity, what);
  if (!key) {
    return false;
  }
  if (auto put = keys_.PutEpochKey(call_id, epoch, *key); !put) {
    log().warning << what << " media key store failed: " << put.error().message;
    return false;
  }
  log().info << what << " media key stored call_id=" << call_id << " epoch=" << epoch
             << " form=" << (media_key_b64.empty() ? "account_wrap" : "in_copy");
  // Mesh answerer Start waits for the epoch key (V015): kick the deferred start.
  if (on_key_ready_) {
    on_key_ready_(call_id);
  }
  return true;
}

Roe<std::optional<CallMediaKeyExchange::TakenKey>> CallMediaKeyExchange::HandleInbound(
    const std::string& detail_json, const std::string& sender_identity) {
  auto key = CallControlCodec::DecodeMediaKey(detail_json);
  if (!key) {
    return key.error();
  }
  log().info << "Inbound CallMediaKey call_id=" << key->call_id << " epoch=" << key->media_epoch
             << " from=" << sender_identity;
  // The key first: the session moves to an epoch only once that epoch's key is here. Moving it first
  // left a call whose key failed to open on an epoch with no key, which a resend could not repair (#381).
  if (!TakeKey(key->call_id, key->media_epoch, key->media_key_id, key->media_key_b64, key->wrapped_key_b64,
               sender_identity, "CallMediaKey")) {
    return std::optional<TakenKey>{};
  }
  return std::optional<TakenKey>{TakenKey{key->call_id, key->media_epoch, key->media_key_id}};
}

} // namespace pbr
