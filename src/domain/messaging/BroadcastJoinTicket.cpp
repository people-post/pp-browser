#include "domain/messaging/BroadcastJoinTicket.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include "common/ValueJson.h"

#include <climits>
#include <sodium.h>
#include <sstream>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

void AppendField(std::ostringstream& out, const char* key, const std::string& value) {
  out << key << '=' << value << '\n';
}
void AppendField(std::ostringstream& out, const char* key, const int64_t value) {
  out << key << '=' << value << '\n';
}
void AppendField(std::ostringstream& out, const char* key, const uint64_t value) {
  out << key << '=' << value << '\n';
}

ByteVector ToBytes(const std::string& text) {
  return ByteVector(text.begin(), text.end());
}

Roe<std::string> MintMediaKeyId() {
  EnsureSodiumInit();
  ByteVector rnd(16);
  randombytes_buf(rnd.data(), rnd.size());
  return std::string("mk:") + BytesToHex(rnd);
}

} // namespace

std::string BroadcastJoinTicketCanonicalSignBytes(const BroadcastJoinTicket& ticket) {
  std::ostringstream out;
  AppendField(out, "v", static_cast<int64_t>(ticket.schema_version));
  AppendField(out, "publisher_peer_id", ticket.publisher_peer_id);
  AppendField(out, "program_id", ticket.program_id);
  AppendField(out, "join_handle", ticket.join_handle);
  AppendField(out, "viewer_peer_id", ticket.viewer_peer_id);
  AppendField(out, "media_epoch", static_cast<uint64_t>(ticket.media_epoch));
  AppendField(out, "media_key_id", ticket.media_key_id);
  AppendField(out, "wrapped_key_b64", ticket.wrapped_key_b64);
  AppendField(out, "key_material_b64", ticket.key_material_b64);
  if (!ticket.hop_peer_id.empty()) {
    AppendField(out, "hop_peer_id", ticket.hop_peer_id);
  }
  AppendField(out, "expires_at_ms", ticket.expires_at_ms);
  return out.str();
}

Roe<std::string> EncodeBroadcastJoinTicketJson(const BroadcastJoinTicket& ticket) {
  Object json;
  json.set("schema_version", static_cast<int64_t>(ticket.schema_version));
  json.set("publisher_peer_id", ticket.publisher_peer_id);
  json.set("program_id", ticket.program_id);
  json.set("join_handle", ticket.join_handle);
  json.set("viewer_peer_id", ticket.viewer_peer_id);
  json.set("media_epoch", static_cast<int64_t>(ticket.media_epoch));
  json.set("media_key_id", ticket.media_key_id);
  if (!ticket.wrapped_key_b64.empty()) {
    json.set("wrapped_key_b64", ticket.wrapped_key_b64);
  }
  if (!ticket.key_material_b64.empty()) {
    json.set("key_material_b64", ticket.key_material_b64);
  }
  if (!ticket.hop_peer_id.empty()) {
    json.set("hop_peer_id", ticket.hop_peer_id);
  }
  json.set("expires_at_ms", ticket.expires_at_ms);
  json.set("signature_b64", ticket.signature_b64);
  return DumpJson(json);
}

Roe<BroadcastJoinTicket> DecodeBroadcastJoinTicketJson(const std::string_view json) {
  auto parsed = ParseObject(std::string(json));
  if (!parsed) {
    return parsed.error();
  }
  const Object& o = *parsed;
  BroadcastJoinTicket ticket;
  ticket.schema_version =
      static_cast<int>(ObjectInt64(o, "schema_version").value_or(kBroadcastJoinTicketSchemaVersion));
  ticket.publisher_peer_id = ObjectString(o, "publisher_peer_id").value_or("");
  ticket.program_id = ObjectString(o, "program_id").value_or("");
  ticket.join_handle = ObjectString(o, "join_handle").value_or("");
  ticket.viewer_peer_id = ObjectString(o, "viewer_peer_id").value_or("");
  const auto epoch = ObjectInt64(o, "media_epoch").value_or(1);
  if (epoch < 0 || epoch > static_cast<int64_t>(UINT32_MAX)) {
    return Error("broadcast join ticket media_epoch out of range");
  }
  ticket.media_epoch = static_cast<uint32_t>(epoch);
  ticket.media_key_id = ObjectString(o, "media_key_id").value_or("");
  ticket.wrapped_key_b64 = ObjectString(o, "wrapped_key_b64").value_or("");
  ticket.key_material_b64 = ObjectString(o, "key_material_b64").value_or("");
  ticket.hop_peer_id = ObjectString(o, "hop_peer_id").value_or("");
  ticket.expires_at_ms = ObjectInt64(o, "expires_at_ms").value_or(0);
  ticket.signature_b64 = ObjectString(o, "signature_b64").value_or("");
  if (ticket.publisher_peer_id.empty() || ticket.join_handle.empty() || ticket.viewer_peer_id.empty()) {
    return Error("broadcast join ticket missing publisher/join_handle/viewer");
  }
  if (ticket.wrapped_key_b64.empty() && ticket.key_material_b64.empty()) {
    return Error("broadcast join ticket missing key material");
  }
  return ticket;
}

Roe<BroadcastJoinTicket> MintBroadcastJoinTicket(BroadcastJoinTicketDraft draft,
                                                 const ByteVector& media_key_bytes,
                                                 const ByteVector& publisher_mldsa_secret_key,
                                                 const ByteVector* viewer_pairwise_session_key) {
  if (draft.publisher_peer_id.empty() || draft.join_handle.empty() || draft.viewer_peer_id.empty()) {
    return Error("broadcast join ticket draft missing publisher/join_handle/viewer");
  }
  if (media_key_bytes.size() != 32) {
    return Error("broadcast media key must be 32 bytes");
  }
  if (publisher_mldsa_secret_key.size() != kMlDsa65SecretKeyBytes) {
    return Error("broadcast join ticket sign requires ML-DSA-65 secret key");
  }
  if (draft.media_epoch == 0) {
    return Error("broadcast join ticket media_epoch must be >= 1");
  }

  BroadcastJoinTicket ticket;
  ticket.schema_version = kBroadcastJoinTicketSchemaVersion;
  ticket.publisher_peer_id = std::move(draft.publisher_peer_id);
  ticket.program_id = std::move(draft.program_id);
  ticket.join_handle = std::move(draft.join_handle);
  ticket.viewer_peer_id = std::move(draft.viewer_peer_id);
  ticket.media_epoch = draft.media_epoch;
  ticket.hop_peer_id = std::move(draft.hop_peer_id);
  ticket.expires_at_ms = draft.expires_at_ms;
  if (draft.media_key_id.empty()) {
    auto id = MintMediaKeyId();
    if (!id) {
      return id.error();
    }
    ticket.media_key_id = *id;
  } else {
    ticket.media_key_id = std::move(draft.media_key_id);
  }

  if (viewer_pairwise_session_key != nullptr && !viewer_pairwise_session_key->empty()) {
    auto wrapped = CallMediaKeyStore::WrapKeyB64(*viewer_pairwise_session_key, media_key_bytes,
                                                 ticket.join_handle, ticket.media_epoch, ticket.media_key_id);
    if (!wrapped) {
      return wrapped.error();
    }
    ticket.wrapped_key_b64 = *wrapped;
  } else {
    ticket.key_material_b64 = Base64Encode(media_key_bytes);
  }

  const auto canonical = ToBytes(BroadcastJoinTicketCanonicalSignBytes(ticket));
  auto signature = MlDsa::Sign(publisher_mldsa_secret_key, canonical);
  if (!signature) {
    return signature.error();
  }
  ticket.signature_b64 = Base64Encode(*signature);
  return ticket;
}

Roe<void> VerifyBroadcastJoinTicket(const BroadcastJoinTicket& ticket,
                                    const ByteVector& publisher_mldsa_public_key, const int64_t now_ms,
                                    const std::string_view expected_viewer_peer_id) {
  if (publisher_mldsa_public_key.size() != kMlDsa65PublicKeyBytes) {
    return Error("broadcast join ticket verify requires ML-DSA-65 public key");
  }
  if (ticket.publisher_peer_id.empty() || ticket.join_handle.empty() || ticket.viewer_peer_id.empty()) {
    return Error("broadcast join ticket missing publisher/join_handle/viewer");
  }
  if (ticket.media_epoch == 0) {
    return Error("broadcast join ticket media_epoch must be >= 1");
  }
  if (ticket.wrapped_key_b64.empty() && ticket.key_material_b64.empty()) {
    return Error("broadcast join ticket missing key material");
  }
  if (ticket.signature_b64.empty()) {
    return Error("broadcast join ticket missing signature");
  }
  if (ticket.expires_at_ms > 0 && now_ms > ticket.expires_at_ms) {
    return Error("broadcast join ticket expired");
  }
  if (!expected_viewer_peer_id.empty() && ticket.viewer_peer_id != expected_viewer_peer_id) {
    return Error("broadcast join ticket viewer mismatch");
  }

  auto signature = Base64Decode(ticket.signature_b64);
  if (!signature) {
    return signature.error();
  }
  if (signature->size() != kMlDsa65SignatureBytes) {
    return Error("broadcast join ticket signature size invalid");
  }
  const auto canonical = ToBytes(BroadcastJoinTicketCanonicalSignBytes(ticket));
  auto ok = MlDsa::Verify(publisher_mldsa_public_key, canonical, *signature);
  if (!ok) {
    return ok.error();
  }
  if (!*ok) {
    return Error("broadcast join ticket signature verify failed");
  }
  return {};
}

Roe<BroadcastMediaKey> ExtractBroadcastMediaKey(const BroadcastJoinTicket& ticket,
                                                const ByteVector& publisher_mldsa_public_key,
                                                const int64_t now_ms,
                                                const std::string_view expected_viewer_peer_id,
                                                const ByteVector* viewer_pairwise_session_key) {
  auto verified =
      VerifyBroadcastJoinTicket(ticket, publisher_mldsa_public_key, now_ms, expected_viewer_peer_id);
  if (!verified) {
    return verified.error();
  }

  ByteVector key_bytes;
  if (!ticket.wrapped_key_b64.empty()) {
    if (viewer_pairwise_session_key == nullptr || viewer_pairwise_session_key->empty()) {
      return Error("broadcast join ticket wrap requires pairwise session key");
    }
    auto unwrapped =
        CallMediaKeyStore::UnwrapKeyB64(*viewer_pairwise_session_key, ticket.wrapped_key_b64,
                                        ticket.join_handle, ticket.media_epoch, ticket.media_key_id);
    if (!unwrapped) {
      return unwrapped.error();
    }
    key_bytes = *unwrapped;
  } else {
    auto decoded = Base64Decode(ticket.key_material_b64);
    if (!decoded) {
      return decoded.error();
    }
    key_bytes = *decoded;
  }
  if (key_bytes.size() != 32) {
    return Error("broadcast media key must be 32 bytes");
  }

  BroadcastMediaKey key;
  key.call_id = ticket.join_handle;
  key.media_epoch = ticket.media_epoch;
  key.media_key_id = ticket.media_key_id;
  key.key_bytes = std::move(key_bytes);
  return key;
}

Roe<BroadcastMediaKey> ApplyBroadcastJoinTicket(CallMediaKeyStore& store, const BroadcastJoinTicket& ticket,
                                                const ByteVector& publisher_mldsa_public_key,
                                                const int64_t now_ms,
                                                const std::string_view expected_viewer_peer_id,
                                                const ByteVector* viewer_pairwise_session_key) {
  auto key = ExtractBroadcastMediaKey(ticket, publisher_mldsa_public_key, now_ms, expected_viewer_peer_id,
                                      viewer_pairwise_session_key);
  if (!key) {
    return key.error();
  }
  auto put = store.PutEpochKey(key->call_id, key->media_epoch, key->key_bytes);
  if (!put) {
    return put.error();
  }
  if (!put->empty()) {
    key->media_key_id = *put;
  }
  return key;
}

} // namespace pbr
