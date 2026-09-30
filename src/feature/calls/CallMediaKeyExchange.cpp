#include "feature/calls/CallMediaKeyExchange.h"

#include "domain/messaging/CallControlCodec.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

CallMediaKeyExchange::CallMediaKeyExchange(CallSessionStore& sessions, CallMediaKeyStore& keys,
                                           CallControlClient& control)
    : sessions_(sessions), keys_(keys), control_(control) {
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
  auto minted = Mint(call_id, (*session)->media_epoch + 1);
  if (!minted) {
    return minted.error();
  }
  (*session)->media_epoch = minted->epoch;
  (*session)->media_key_id = minted->key_id;
  if (auto saved = sessions_.UpsertSession(**session); !saved) {
    return saved.error();
  }
  return minted;
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
  auto session_key = control_.ResolvePeerSessionKey(peer_identity);
  if (!session_key) {
    log().warning << "Media key wrap skip; no peer session key call_id=" << call_id << " peer=" << peer_identity
                  << " err=" << session_key.error().message;
    return {};
  }
  auto wrapped = CallMediaKeyStore::WrapKeyB64(*session_key, key.key, call_id, key.epoch, key.key_id);
  if (!wrapped) {
    log().warning << "Media key wrap failed call_id=" << call_id << " err=" << wrapped.error().message;
    return {};
  }
  return std::move(*wrapped);
}

Roe<void> CallMediaKeyExchange::Send(const std::string& call_id, const std::string& peer_identity,
                                     const EpochKey& key) {
  if (auto sent = control_.SendMediaKey(call_id, peer_identity, key.epoch, key.key_id, key.key); !sent) {
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

bool CallMediaKeyExchange::TakeWrapped(const std::string& call_id, const uint32_t epoch, const std::string& key_id,
                                       const std::string& wrapped_key_b64, const std::string& sender_identity,
                                       const char* what) {
  if (wrapped_key_b64.empty()) {
    return false;
  }
  auto session_key = control_.ResolvePeerSessionKey(sender_identity);
  if (!session_key) {
    log().warning << what << " media key missing peer session key from=" << sender_identity;
    return false;
  }
  auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(*session_key, wrapped_key_b64, call_id, epoch, key_id);
  if (!unwrapped) {
    log().warning << what << " media key unwrap failed: " << unwrapped.error().message;
    return false;
  }
  if (auto put = keys_.PutEpochKey(call_id, epoch, *unwrapped); !put) {
    log().warning << what << " media key store failed: " << put.error().message;
    return false;
  }
  log().info << what << " media key stored call_id=" << call_id << " epoch=" << epoch;
  // Mesh answerer Start waits for the epoch key (V015): kick the deferred start.
  if (on_key_ready_) {
    on_key_ready_(call_id);
  }
  return true;
}

Roe<void> CallMediaKeyExchange::HandleInbound(const std::string& detail_json, const std::string& sender_identity) {
  auto key = CallControlCodec::DecodeMediaKey(detail_json);
  if (!key) {
    return key.error();
  }
  log().info << "Inbound CallMediaKey call_id=" << key->call_id << " epoch=" << key->media_epoch
             << " from=" << sender_identity;
  auto session = sessions_.LoadSession(key->call_id);
  if (session && session->has_value()) {
    (*session)->media_epoch = key->media_epoch;
    (*session)->media_key_id = key->media_key_id;
    (void)sessions_.UpsertSession(**session);
  }
  (void)TakeWrapped(key->call_id, key->media_epoch, key->media_key_id, key->wrapped_key_b64, sender_identity,
                    "CallMediaKey");
  return {};
}

} // namespace pbr
