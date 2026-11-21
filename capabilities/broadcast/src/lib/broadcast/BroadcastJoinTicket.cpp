#include "lib/broadcast/BroadcastJoinTicket.h"

#include "lib/broadcast/BroadcastSignedJson.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"

#include "common/ValueJson.h"

#include <climits>
#include <limits>
#include <sodium.h>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr char kWhat[] = "broadcast join ticket";
constexpr SignedVersions kTicketVersions{.floor = kBroadcastJoinTicketSchemaVersion,
                                         .current = kBroadcastJoinTicketSchemaVersion};

Roe<std::string> MintMediaKeyId() {
  EnsureSodiumInit();
  ByteVector rnd(16);
  randombytes_buf(rnd.data(), rnd.size());
  return std::string("mk:") + BytesToHex(rnd);
}

/** The key fields' bytes, decoded once for the signed bytes and the algorithm step. */
struct TicketKeyBytes {
  ByteVector wrapped_key;
  ByteVector key_material;
};

Roe<TicketKeyBytes> DecodeKeyBytes(const BroadcastJoinTicket& ticket) {
  auto wrapped = ReadOptionalBase64(ticket.wrapped_key_b64, "broadcast join ticket wrapped_key_b64");
  if (!wrapped) {
    return wrapped.error();
  }
  auto material = ReadOptionalBase64(ticket.key_material_b64, "broadcast join ticket key_material_b64");
  if (!material) {
    return material.error();
  }
  return TicketKeyBytes{.wrapped_key = std::move(*wrapped), .key_material = std::move(*material)};
}

/** Step 2's bounds, for a ticket decoded or built in memory. */
Roe<void> CheckTicketFields(const BroadcastJoinTicket& ticket) {
  if (ticket.publisher_peer_id.empty() || ticket.join_handle.empty() || ticket.viewer_peer_id.empty()) {
    return Error("broadcast join ticket missing publisher/join_handle/viewer");
  }
  if (ticket.media_epoch == 0) {
    return Error("broadcast join ticket media_epoch must be >= 1");
  }
  if (ticket.wrapped_key_b64.empty() && ticket.key_material_b64.empty()) {
    return Error("broadcast join ticket missing key material");
  }
  return {};
}

ByteVector SignBytesOf(const BroadcastJoinTicket& ticket, const uint8_t version, const TicketKeyBytes& keys) {
  return SignedBytesWriter(kBroadcastJoinTicketDomain, version)
      .Utf8(ticket.publisher_peer_id)
      .Utf8(ticket.program_id)
      .Utf8(ticket.join_handle)
      .Utf8(ticket.viewer_peer_id)
      .U64(ticket.media_epoch)
      .Utf8(ticket.media_key_id)
      .Bytes(keys.wrapped_key)
      .Bytes(keys.key_material)
      .Utf8(ticket.hop_peer_id)
      .I64(ticket.expires_at_ms)
      .Finish(ticket.ext);
}

} // namespace

Roe<ByteVector> BroadcastJoinTicketSignBytes(const BroadcastJoinTicket& ticket) {
  if (ticket.schema_version < 0 || ticket.schema_version > std::numeric_limits<uint8_t>::max()) {
    return Error("broadcast join ticket schema_version is not a byte");
  }
  auto keys = DecodeKeyBytes(ticket);
  if (!keys) {
    return keys.error();
  }
  return SignBytesOf(ticket, static_cast<uint8_t>(ticket.schema_version), *keys);
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
  WriteExtB64(json, ticket.ext);
  json.set("signature_b64", ticket.signature_b64);
  return DumpJson(json);
}

namespace {

/** Step 2 from the wire: the fields of a ticket whose `schema_version` (`version`) is in range. */
Roe<BroadcastJoinTicket> ParseTicketFields(const Object& o, const int version) {
  BroadcastJoinTicket ticket;
  ticket.schema_version = version;
  for (auto [key, field] :
       {std::pair{"publisher_peer_id", &ticket.publisher_peer_id}, std::pair{"program_id", &ticket.program_id},
        std::pair{"join_handle", &ticket.join_handle}, std::pair{"viewer_peer_id", &ticket.viewer_peer_id},
        std::pair{"media_key_id", &ticket.media_key_id}, std::pair{"wrapped_key_b64", &ticket.wrapped_key_b64},
        std::pair{"key_material_b64", &ticket.key_material_b64}, std::pair{"hop_peer_id", &ticket.hop_peer_id},
        std::pair{"signature_b64", &ticket.signature_b64}}) {
    auto value = ReadOptionalString(o, key, kWhat);
    if (!value) {
      return value.error();
    }
    *field = std::move(*value);
  }
  auto epoch = ReadOptionalInt(o, "media_epoch", 0, kWhat);
  if (!epoch) {
    return epoch.error();
  }
  if (*epoch < 0 || *epoch > static_cast<int64_t>(UINT32_MAX)) {
    return Error("broadcast join ticket media_epoch out of range");
  }
  ticket.media_epoch = static_cast<uint32_t>(*epoch);
  auto expires = ReadOptionalInt(o, "expires_at_ms", 0, kWhat);
  if (!expires) {
    return expires.error();
  }
  ticket.expires_at_ms = *expires;
  auto ext = ReadExtB64(o);
  if (!ext) {
    return Error(std::string(kWhat) + ": " + ext.error().message);
  }
  ticket.ext = std::move(*ext);
  if (auto fields = CheckTicketFields(ticket); !fields) {
    return fields.error();
  }
  if (auto keys = DecodeKeyBytes(ticket); !keys) {
    return keys.error();
  }
  return ticket;
}

/**
 * Steps 3–5 for a parsed ticket (`*ticket`, filled by the caller's parse step, which also fills `keys`): the
 * signature's and the wrapped key's algorithm ids, the publisher's key, the signature, the extension area.
 */
struct TicketReceive {
  const BroadcastJoinTicket* ticket = nullptr;
  TypedRead publisher_key;
  TicketKeyBytes keys;
  TypedRead signature;

  Roe<void> Algorithms() {
    signature = ReadTypedBlobB64(pp::BlobKind::kSignature, ticket->signature_b64, "broadcast join ticket signature");
    if (!signature.ok()) {
      return signature.ToError();
    }
    if (!keys.wrapped_key.empty()) {
      if (const TypedRead wrapped =
              ReadTypedBlob(pp::BlobKind::kAeadSealed, keys.wrapped_key, "broadcast join ticket wrapped key");
          !wrapped.ok()) {
        return wrapped.ToError();
      }
    }
    if (!publisher_key.ok()) {
      return publisher_key.ToError();
    }
    return {};
  }

  Roe<void> Verify() const {
    return VerifyTyped(publisher_key, SignBytesOf(*ticket, static_cast<uint8_t>(ticket->schema_version), keys),
                       signature);
  }
};

} // namespace

Roe<BroadcastJoinTicket> DecodeBroadcastJoinTicketJson(const std::string_view json) {
  auto parsed = ParseObject(std::string(json));
  if (!parsed) {
    return parsed.error();
  }
  // 1. The version, before any field is read; 2. the fields.
  auto version = ReadSchemaVersion(*parsed, kWhat, kBroadcastJoinTicketSchemaVersion);
  if (!version) {
    return version.error();
  }
  return ParseTicketFields(*parsed, *version);
}

SignedReceipt ReceiveBroadcastJoinTicketJson(const std::string_view json, const ByteVector& publisher_mldsa_public_key,
                                             BroadcastJoinTicket* out) {
  auto parsed = ParseObject(std::string(json));
  if (!parsed) {
    return SignedReceipt{.verdict = SignedVerdict::Malformed, .version = 0, .detail = parsed.error().message};
  }
  BroadcastJoinTicket ticket;
  TicketReceive receive{.ticket = &ticket, .publisher_key = PublisherSigningKey(publisher_mldsa_public_key)};
  const std::optional<int64_t> version = OutsideVersionOfJson(*parsed, "schema_version");
  SignedReceipt receipt = ReceiveSigned({
      .what = kWhat,
      .outside_version = version,
      .versions = kTicketVersions,
      .parse = [&]() -> Roe<void> {
        auto fields = ParseTicketFields(*parsed, static_cast<int>(*version));
        if (!fields) {
          return fields.error();
        }
        ticket = std::move(*fields);
        receive.keys = DecodeKeyBytes(ticket).value();
        return {};
      },
      .algorithms = [&] { return receive.Algorithms(); },
      .verify = [&] { return receive.Verify(); },
      .ext = &ticket.ext,
  });
  if (receipt.accepted() && out != nullptr) {
    *out = std::move(ticket);
  }
  return receipt;
}

Roe<BroadcastJoinTicket> MintBroadcastJoinTicket(BroadcastJoinTicketDraft draft, const ByteVector& media_key_bytes,
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
    auto wrapped = WrapMediaKeyB64(kBroadcastMediaKeyWrapLabel, *viewer_pairwise_session_key, media_key_bytes,
                                   ticket.join_handle, ticket.media_epoch, ticket.media_key_id);
    if (!wrapped) {
      return wrapped.error();
    }
    ticket.wrapped_key_b64 = *wrapped;
  } else {
    ticket.key_material_b64 = Base64Encode(media_key_bytes);
  }

  auto sign_bytes = BroadcastJoinTicketSignBytes(ticket);
  if (!sign_bytes) {
    return sign_bytes.error();
  }
  auto signature = SignTyped(publisher_mldsa_secret_key, *sign_bytes);
  if (!signature) {
    return signature.error();
  }
  ticket.signature_b64 = Base64Encode(*signature);
  return ticket;
}

SignedReceipt ReceiveBroadcastJoinTicket(const BroadcastJoinTicket& ticket,
                                         const ByteVector& publisher_mldsa_public_key) {
  TicketReceive receive{.ticket = &ticket, .publisher_key = PublisherSigningKey(publisher_mldsa_public_key)};
  return ReceiveSigned({
      .what = kWhat,
      .outside_version = ticket.schema_version,
      .versions = kTicketVersions,
      .parse = [&]() -> Roe<void> {
        if (auto fields = CheckTicketFields(ticket); !fields) {
          return fields;
        }
        auto keys = DecodeKeyBytes(ticket);
        if (!keys) {
          return keys.error();
        }
        receive.keys = std::move(*keys);
        return {};
      },
      .algorithms = [&] { return receive.Algorithms(); },
      .verify = [&] { return receive.Verify(); },
      .ext = &ticket.ext,
  });
}

Roe<void> VerifyBroadcastJoinTicket(const BroadcastJoinTicket& ticket, const ByteVector& publisher_mldsa_public_key,
                                    const int64_t now_ms, const std::string_view expected_viewer_peer_id) {
  if (const SignedReceipt receipt = ReceiveBroadcastJoinTicket(ticket, publisher_mldsa_public_key);
      !receipt.accepted()) {
    return receipt.ToError();
  }
  // 6. Semantics.
  if (ticket.expires_at_ms > 0 && now_ms > ticket.expires_at_ms) {
    return Error("broadcast join ticket expired");
  }
  if (!expected_viewer_peer_id.empty() && ticket.viewer_peer_id != expected_viewer_peer_id) {
    return Error("broadcast join ticket viewer mismatch");
  }
  return {};
}

Roe<BroadcastMediaKey> ExtractBroadcastMediaKey(const BroadcastJoinTicket& ticket,
                                                const ByteVector& publisher_mldsa_public_key, const int64_t now_ms,
                                                const std::string_view expected_viewer_peer_id,
                                                const ByteVector* viewer_pairwise_session_key) {
  auto verified = VerifyBroadcastJoinTicket(ticket, publisher_mldsa_public_key, now_ms, expected_viewer_peer_id);
  if (!verified) {
    return verified.error();
  }

  ByteVector key_bytes;
  if (!ticket.wrapped_key_b64.empty()) {
    if (viewer_pairwise_session_key == nullptr || viewer_pairwise_session_key->empty()) {
      return Error("broadcast join ticket wrap requires pairwise session key");
    }
    auto unwrapped =
        UnwrapMediaKeyB64(kBroadcastMediaKeyWrapLabel, *viewer_pairwise_session_key, ticket.wrapped_key_b64,
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

} // namespace pbr
