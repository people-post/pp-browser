#include "lib/delivery/PeerRequestAuth.h"

#include "lib/people/PeerSigningKeyStore.h"
#include "lib/people/AccountCard.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"
#include "common/thread/ThreadChannel.h"
#include "common/directory/DirectoryJson.h"

#include <cstdlib>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr::PeerRequestAuth {

namespace {

constexpr SignedVersions kVersions{.floor = kVersion, .current = kVersion};

/** The header every kind shares: the domain, the version the request carries, who asks, from where, when. */
template <typename Request>
Roe<SignedBytesWriter> Begin(const char* kind, const Request& request) {
  if (!request.auth_version || *request.auth_version != kVersion) {
    return Error("Peer request auth version " + std::to_string(request.auth_version.value_or(0)) + " is not built");
  }
  SignedBytesWriter out(kDomain, static_cast<uint8_t>(*request.auth_version));
  out.Utf8(kind).Utf8(request.requester_account_id).Utf8(request.requester_peer_id).I64(request.timestamp_ms);
  return out;
}

template <typename Request>
Roe<void> SignImpl(Request& request, const std::string& account_id, const std::string& local_peer_id,
                   const int64_t now_ms, const SignFn& sign) {
  if (account_id.empty() || local_peer_id.empty()) {
    return Error("Peer request signer missing account or peer id");
  }
  request.auth_version = kVersion;
  request.requester_account_id = account_id;
  request.requester_peer_id = local_peer_id;
  request.timestamp_ms = now_ms;
  request.ext = pp::ExtArea{};
  request.ext_error.clear();
  auto bytes = SignBytes(request);
  if (!bytes) {
    return bytes.error();
  }
  auto signature = SignTypedB64(sign, *bytes);
  if (!signature) {
    return signature.error();
  }
  request.signature = std::move(*signature);
  return {};
}

/** The receiver's order (SIGNED_FORMATS § 1.1): version, parse, algorithm ids, verify, extensions. */
template <typename Request>
SignedReceipt ReceiveImpl(const Request& request, const SigningKeySource& signing_key_b64) {
  TypedRead key;
  TypedRead signature;
  return ReceiveSigned(SignedReceiveSteps{
      .what = "peer request",
      .outside_version = request.auth_version,
      .versions = kVersions,
      .parse = [&]() -> Roe<void> {
        if (!request.ext_error.empty()) {
          return Error(request.ext_error);
        }
        if (request.signature.empty() || request.requester_account_id.empty() || request.requester_peer_id.empty()) {
          return Error("missing auth fields");
        }
        return {};
      },
      .algorithms = [&]() -> Roe<void> {
        signature = ReadTypedBlobB64(pp::BlobKind::kSignature, request.signature, "peer request signature");
        if (!signature.ok()) {
          return signature.ToError();
        }
        auto key_b64 = signing_key_b64();
        if (!key_b64) {
          return Error("no signing key for the requester");
        }
        key = ReadHeldSigningKeyB64(*key_b64, "requester signing key");
        if (!key.ok()) {
          return key.ToError();
        }
        return {};
      },
      .verify = [&]() -> Roe<void> {
        auto bytes = SignBytes(request);
        if (!bytes) {
          return bytes.error();
        }
        return VerifyTyped(key, *bytes, signature);
      },
      .ext = &request.ext});
}

/** Steps 1–5, then the request's own semantics (step 6): the link it signed for, and its time. */
template <typename Request>
Roe<void> VerifyImpl(const Request& request, const std::string& remote_peer_id, const int64_t now_ms,
                     const SigningKeySource& signing_key_b64) {
  const SignedReceipt receipt = ReceiveImpl(request, signing_key_b64);
  if (receipt.needs_update()) {
    return receipt.ToError(); // AppError::NewerFormat: the server answers update_needed
  }
  // One answer for every other refusal: the reason stays with us, not with the asker.
  if (!receipt.accepted() || remote_peer_id.empty() || request.requester_peer_id != remote_peer_id ||
      std::llabs(now_ms - request.timestamp_ms) > kMaxSkewMs) {
    return Error("Request not authorized");
  }
  return {};
}

SigningKeySource ResolvedKey(IPeerSigningKeyResolver& keys, const std::string& account_id) {
  return [&keys, account_id]() -> Roe<std::string> {
    auto key = keys.Resolve(ContactIdKindToString(mesh::ContactIdKind::Account), account_id);
    if (!key) {
      return key.error();
    }
    return key->signing_public_key_b64;
  };
}

} // namespace

SignedReceipt Receive(const ChatHistoryRequest& request, const SigningKeySource& signing_key_b64) {
  return ReceiveImpl(request, signing_key_b64);
}

SignedReceipt Receive(const ChatBlobRequest& request, const SigningKeySource& signing_key_b64) {
  return ReceiveImpl(request, signing_key_b64);
}

SignedReceipt Receive(const AccountCardRequest& request, const SigningKeySource& signing_key_b64) {
  return ReceiveImpl(request, signing_key_b64);
}

Roe<std::vector<uint8_t>> SignBytes(const ChatHistoryRequest& request) {
  auto out = Begin("history", request);
  if (!out) {
    return out.error();
  }
  out->Utf8(request.requester_identity_kind).Utf8(request.requester_identity_value);
  out->Utf8(request.peer_identity_kind).Utf8(request.peer_identity_value);
  out->Utf8(ThreadChannelToString(request.channel)).U32(request.session_epoch);
  out->U64(request.min_sender_seq.value_or(0)).U64(request.max_sender_seq.value_or(UINT64_MAX));
  out->U64(static_cast<uint64_t>(request.limit)).Utf8(request.order);
  return out->Finish(request.ext);
}

Roe<std::vector<uint8_t>> SignBytes(const ChatBlobRequest& request) {
  auto out = Begin("blob", request);
  if (!out) {
    return out.error();
  }
  out->U8(static_cast<uint8_t>(request.op));
  out->Utf8(request.requester_identity_kind).Utf8(request.requester_identity_value);
  out->Utf8(request.peer_identity_kind).Utf8(request.peer_identity_value);
  out->Utf8(request.thread_id).Utf8(request.content_hash_hex).Utf8(ThreadChannelToString(request.channel));
  return out->Finish(request.ext);
}

Roe<std::vector<uint8_t>> SignBytes(const AccountCardRequest& request) {
  auto out = Begin("account-card", request);
  if (!out) {
    return out.error();
  }
  out->Utf8(request.account_id);
  return out->Finish(request.ext);
}

Roe<void> Sign(ChatHistoryRequest& request, const std::string& account_id, const std::string& local_peer_id,
               const int64_t now_ms, const SignFn& sign) {
  return SignImpl(request, account_id, local_peer_id, now_ms, sign);
}

Roe<void> Sign(ChatBlobRequest& request, const std::string& account_id, const std::string& local_peer_id,
               const int64_t now_ms, const SignFn& sign) {
  return SignImpl(request, account_id, local_peer_id, now_ms, sign);
}

Roe<void> Sign(AccountCardRequest& request, const std::string& account_id, const std::string& local_peer_id,
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
