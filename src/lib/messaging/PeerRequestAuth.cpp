#include "lib/messaging/PeerRequestAuth.h"

#include "lib/messaging/PeerSigningKeyStore.h"
#include "lib/messaging/AccountCard.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "common/thread/ThreadChannel.h"
#include "common/directory/DirectoryJson.h"

#include "common/Serialize.hpp"
#include "crypto/PeerId.h"

#include <cstdlib>
#include <sstream>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr::PeerRequestAuth {

namespace {

constexpr char kDomain[] = "pp-browser:peer-request-sign-v1";

void Put(OutputArchive& ar, const std::string& value) {
  WireLenUtf8 field{value};
  ar & field;
}

std::ostringstream Begin(const char* kind, const std::string& requester_account_id,
                         const std::string& requester_peer_id, const int64_t timestamp_ms) {
  std::ostringstream oss;
  oss.write(kDomain, sizeof(kDomain));  // with its NUL
  OutputArchive ar(oss);
  Put(ar, kind);
  Put(ar, requester_account_id);
  Put(ar, requester_peer_id);
  ar & timestamp_ms;
  return oss;
}

std::vector<uint8_t> Bytes(const std::ostringstream& oss) {
  const std::string packed = oss.str();
  return std::vector<uint8_t>(packed.begin(), packed.end());
}

template <typename Request>
Roe<void> SignImpl(Request& request, const std::string& account_id, const std::string& local_peer_id,
                   const int64_t now_ms, const SignFn& sign) {
  if (account_id.empty() || local_peer_id.empty()) {
    return Error("Peer request signer missing account or peer id");
  }
  request.requester_account_id = account_id;
  request.requester_peer_id = local_peer_id;
  request.timestamp_ms = now_ms;
  auto bytes = SignBytes(request);
  if (!bytes) {
    return bytes.error();
  }
  auto signature = sign(*bytes);
  if (!signature) {
    return signature.error();
  }
  request.signature = *signature;
  return {};
}

template <typename Request>
Roe<void> VerifyImpl(const Request& request, const std::string& remote_peer_id, const int64_t now_ms,
                     const std::function<Roe<std::string>()>& signing_key_b64) {
  // One answer for every refusal: the reason stays in our log, not with the asker.
  if (request.signature.empty() || request.requester_account_id.empty() || remote_peer_id.empty() ||
      request.requester_peer_id != remote_peer_id || std::llabs(now_ms - request.timestamp_ms) > kMaxSkewMs) {
    return Error("Request not authorized");
  }
  auto key = signing_key_b64();
  if (!key) {
    return Error("Request not authorized");
  }
  auto bytes = SignBytes(request);
  auto public_key = Base64Decode(*key);
  auto signature = Base64Decode(request.signature);
  if (!bytes || !public_key || !signature) {
    return Error("Request not authorized");
  }
  auto verified = MlDsa::Verify(*public_key, *bytes, *signature);
  if (!verified || !*verified) {
    return Error("Request not authorized");
  }
  return {};
}

std::function<Roe<std::string>()> ResolvedKey(IPeerSigningKeyResolver& keys, const std::string& account_id) {
  return [&keys, account_id]() -> Roe<std::string> {
    auto key = keys.Resolve(ContactIdKindToString(mesh::ContactIdKind::Account), account_id);
    if (!key) {
      return key.error();
    }
    return key->signing_public_key_b64;
  };
}

} // namespace

Roe<std::string> DevicePeerId(const std::string& device_public_key_b64) {
  auto key = Base64Decode(device_public_key_b64);
  if (!key) {
    return key.error();
  }
  return pp::PeerIdFromMlDsaPublicKey(*key);
}

Roe<std::vector<uint8_t>> SignBytes(const ChatHistoryRequest& request) {
  auto oss = Begin("history", request.requester_account_id, request.requester_peer_id, request.timestamp_ms);
  OutputArchive ar(oss);
  Put(ar, request.requester_identity_kind);
  Put(ar, request.requester_identity_value);
  Put(ar, request.peer_identity_kind);
  Put(ar, request.peer_identity_value);
  Put(ar, ThreadChannelToString(request.channel));
  ar & request.session_epoch;
  const uint64_t min_seq = request.min_sender_seq.value_or(0);
  const uint64_t max_seq = request.max_sender_seq.value_or(UINT64_MAX);
  ar & min_seq;
  ar & max_seq;
  const uint64_t limit = request.limit;
  ar & limit;
  Put(ar, request.order);
  return Bytes(oss);
}

Roe<std::vector<uint8_t>> SignBytes(const ChatBlobRequest& request) {
  auto oss = Begin("blob", request.requester_account_id, request.requester_peer_id, request.timestamp_ms);
  OutputArchive ar(oss);
  const uint8_t op = static_cast<uint8_t>(request.op);
  ar & op;
  Put(ar, request.requester_identity_kind);
  Put(ar, request.requester_identity_value);
  Put(ar, request.peer_identity_kind);
  Put(ar, request.peer_identity_value);
  Put(ar, request.thread_id);
  Put(ar, request.content_hash_hex);
  Put(ar, ThreadChannelToString(request.channel));
  return Bytes(oss);
}

Roe<void> Sign(ChatHistoryRequest& request, const std::string& account_id, const std::string& local_peer_id,
               const int64_t now_ms, const SignFn& sign) {
  return SignImpl(request, account_id, local_peer_id, now_ms, sign);
}

Roe<void> Sign(ChatBlobRequest& request, const std::string& account_id, const std::string& local_peer_id,
               const int64_t now_ms, const SignFn& sign) {
  return SignImpl(request, account_id, local_peer_id, now_ms, sign);
}

Roe<void> Verify(const ChatHistoryRequest& request, const std::string& remote_peer_id, const int64_t now_ms,
                 IPeerSigningKeyResolver& keys) {
  return VerifyImpl(request, remote_peer_id, now_ms, ResolvedKey(keys, request.requester_account_id));
}

Roe<void> Verify(const ChatBlobRequest& request, const std::string& remote_peer_id, const int64_t now_ms,
                 IPeerSigningKeyResolver& keys) {
  return VerifyImpl(request, remote_peer_id, now_ms, ResolvedKey(keys, request.requester_account_id));
}

Roe<std::vector<uint8_t>> SignBytes(const AccountCardRequest& request) {
  auto oss = Begin("account-card", request.requester_account_id, request.requester_peer_id, request.timestamp_ms);
  OutputArchive ar(oss);
  Put(ar, request.account_id);
  return Bytes(oss);
}

Roe<void> Sign(AccountCardRequest& request, const std::string& account_id, const std::string& local_peer_id,
               const int64_t now_ms, const SignFn& sign) {
  return SignImpl(request, account_id, local_peer_id, now_ms, sign);
}

Roe<void> Verify(const AccountCardRequest& request, const std::string& remote_peer_id, const int64_t now_ms,
                 IPeerSigningKeyResolver& keys) {
  // The requester's own key card, when it proves itself for the requester: no lookup for a stranger.
  if (request.requester_keys && request.requester_keys->account_id == request.requester_account_id &&
      VerifyKeyCard(*request.requester_keys)) {
    const std::string key = request.requester_keys->signing_public_key_b64;
    return VerifyImpl(request, remote_peer_id, now_ms, [key]() -> Roe<std::string> { return key; });
  }
  return VerifyImpl(request, remote_peer_id, now_ms, ResolvedKey(keys, request.requester_account_id));
}

} // namespace pbr::PeerRequestAuth
