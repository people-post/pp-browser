#include "base/p2p/PeerIdUtil.h"

#include <libp2p/peer/peer_id.hpp>
#include <libp2p/wire/keys_wire.hpp>

namespace pbr {

namespace {

constexpr size_t kEd25519PublicKeyBytes = 32;

} // namespace

Roe<std::string> PeerIdFromEd25519PublicKey(const std::vector<uint8_t>& public_key) {
  if (public_key.size() != kEd25519PublicKeyBytes) {
    return Error("Ed25519 public key must be 32 bytes");
  }

  libp2p::wire::PublicKeyWire wire_key;
  wire_key.type = libp2p::wire::KeyTypeWire::kEd25519;
  wire_key.data.assign(public_key.begin(), public_key.end());

  auto encoded = wire_key.encode();
  if (!encoded) {
    return Error("Failed to encode Ed25519 public key");
  }

  const libp2p::crypto::ProtobufKey marshalled{
      {encoded.value().begin(), encoded.value().end()}};

  auto peer_id = libp2p::peer::PeerId::fromPublicKey(marshalled);
  if (!peer_id) {
    return Error("Failed to derive PeerId from Ed25519 public key");
  }
  return peer_id.value().toBase58();
}

} // namespace pbr
