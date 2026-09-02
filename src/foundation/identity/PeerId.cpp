#include "foundation/identity/PeerId.h"

#include "foundation/identity/Base58.h"

#include <openssl/sha.h>

#include <array>

namespace pbr {

namespace {

std::array<uint8_t, SHA256_DIGEST_LENGTH> Sha256(std::span<const uint8_t> input) {
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  SHA256(input.data(), input.size(), digest.data());
  return digest;
}

} // namespace

PeerId PeerId::FromProtobufPublicKey(std::span<const uint8_t> protobuf_key) {
  if (protobuf_key.size() <= kMaxInlineKeyLength) {
    return PeerId(Multihash::Create(HashType::kIdentity,
                                    std::vector<uint8_t>(protobuf_key.begin(), protobuf_key.end())));
  }

  const auto digest = Sha256(protobuf_key);
  return PeerId(Multihash::Create(HashType::kSha256,
                                  std::span<const uint8_t>(digest.data(), digest.size())));
}

PeerId::PeerId(Multihash hash) : hash_(std::move(hash)) {}

std::string PeerId::ToBase58() const {
  return EncodeBase58(hash_.buffer());
}

} // namespace pbr
