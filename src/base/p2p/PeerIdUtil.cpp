#include "base/p2p/PeerIdUtil.h"

#include "base/crypto/MlDsa.h"

#include <libp2p/peer/peer_id.hpp>
#include <libp2p/wire/keys_wire.hpp>

namespace pbr {

Roe<std::string> PeerIdFromMlDsaPublicKey(const std::vector<uint8_t>& public_key) {
  if (public_key.size() != kMlDsa65PublicKeyBytes) {
    return Error("ML-DSA-65 public key must be 1952 bytes");
  }

  libp2p::wire::PublicKeyWire wire_key;
  wire_key.type = libp2p::wire::KeyTypeWire::kMlDsa65;
  wire_key.data.assign(public_key.begin(), public_key.end());

  auto encoded = wire_key.encode();
  if (!encoded) {
    return Error("Failed to encode ML-DSA-65 public key");
  }

  const libp2p::crypto::ProtobufKey marshalled{
      {encoded.value().begin(), encoded.value().end()}};

  auto peer_id = libp2p::peer::PeerId::fromPublicKey(marshalled);
  if (!peer_id) {
    return Error("Failed to derive PeerId from ML-DSA-65 public key");
  }
  return peer_id.value().toBase58();
}

} // namespace pbr
