#include "lib/net/RegistrationSignPayload.h"

#include "foundation/crypto/TypedBlob.h"
#include "lib/net/RelaySignBytes.h"

namespace pbr {

namespace {

/** A typed blob as it was read: its id byte, then the algorithm's bytes. */
ByteVector TypedBytes(const TypedRead& read) {
  ByteVector bytes{read.alg_id};
  bytes.insert(bytes.end(), read.raw.begin(), read.raw.end());
  return bytes;
}

} // namespace

std::vector<uint8_t> BuildRegistrationSignBytes(const std::string& audience, const std::string& challenge,
                                                const std::string& typed_public_key_b64,
                                                const std::string& typed_kem_public_key_b64, const int64_t timestamp,
                                                const pp::ExtArea& ext) {
  const TypedRead public_key =
      ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, typed_public_key_b64, "registration signing key");
  const TypedRead kem_public_key =
      ReadTypedBlobB64(pp::BlobKind::kKemPublicKey, typed_kem_public_key_b64, "registration KEM key");
  if (!public_key.ok() || !kem_public_key.ok()) {
    return {};
  }
  return BuildRelayCallBytes(kRelayRegisterDomain, audience, ext, [&](SignedBytesWriter& out) {
    out.Utf8(challenge).Typed(TypedBytes(public_key)).Typed(TypedBytes(kem_public_key)).I64(timestamp);
  });
}

std::vector<uint8_t> BuildProfileUpdateSignBytes(const std::string& audience, const std::string& account_id,
                                                 const std::string& nickname, const int64_t timestamp,
                                                 const pp::ExtArea& ext) {
  return BuildRelayCallBytes(kRelayProfileDomain, audience, ext,
                             [&](SignedBytesWriter& out) { out.Utf8(account_id).Utf8(nickname).I64(timestamp); });
}

std::vector<uint8_t> BuildAccountRevokeSignBytes(const std::string& audience, const std::string& account_id,
                                                 const std::string& registration_id, const int64_t timestamp,
                                                 const pp::ExtArea& ext) {
  return BuildRelayCallBytes(kRelayRevokeDomain, audience, ext, [&](SignedBytesWriter& out) {
    out.Utf8(account_id).Utf8(registration_id).I64(timestamp);
  });
}

std::vector<uint8_t> BuildRelayHeartbeatSignBytes(const std::string& audience, const std::string& account_id,
                                                  const std::string& peer_id, const std::string& state,
                                                  const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildRelayCallBytes(kRelayHeartbeatDomain, audience, ext, [&](SignedBytesWriter& out) {
    out.Utf8(account_id).Utf8(peer_id).Utf8(state).I64(timestamp);
  });
}

std::vector<uint8_t> BuildLlmTokenRenewSignBytes(const std::string& audience, const std::string& account_id,
                                                 const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildRelayCallBytes(kRelayLlmTokenDomain, audience, ext,
                             [&](SignedBytesWriter& out) { out.Utf8(account_id).I64(timestamp); });
}

} // namespace pbr
