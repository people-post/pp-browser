#include "base/mesh/identity/PeerIdUtil.h"

#include "base/crypto/MlDsa.h"
#include "base/mesh/identity/MlDsaPublicKeyWire.h"
#include "base/mesh/identity/PeerId.h"

namespace pbr {

Roe<std::string> PeerIdFromMlDsaPublicKey(const std::vector<uint8_t>& public_key) {
  if (public_key.size() != kMlDsa65PublicKeyBytes) {
    return Error("ML-DSA-65 public key must be 1952 bytes");
  }

  const auto encoded = EncodeMlDsa65PublicKeyWire(public_key);
  if (encoded.empty()) {
    return Error("Failed to encode ML-DSA-65 public key");
  }

  return PeerId::FromProtobufPublicKey(encoded).ToBase58();
}

} // namespace pbr
