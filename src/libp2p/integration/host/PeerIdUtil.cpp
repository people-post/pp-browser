#include "libp2p/integration/host/PeerIdUtil.h"

#include <generated/crypto/protobuf/keys.pb.h>
#include <libp2p/crypto/protobuf/protobuf_key.hpp>
#include <libp2p/peer/peer_id.hpp>

namespace pbr {

namespace {

constexpr size_t kEd25519PublicKeyBytes = 32;

} // namespace

Roe<std::string> PeerIdFromEd25519PublicKey(const std::vector<uint8_t>& public_key) {
  if (public_key.size() != kEd25519PublicKeyBytes) {
    return Error("Ed25519 public key must be 32 bytes");
  }

  libp2p::crypto::protobuf::PublicKey protobuf_key;
  protobuf_key.set_type(libp2p::crypto::protobuf::KeyType::Ed25519);
  protobuf_key.set_data(public_key.data(), public_key.size());

  const std::string serialized = protobuf_key.SerializeAsString();
  const libp2p::crypto::ProtobufKey marshalled{{serialized.begin(), serialized.end()}};

  auto peer_id = libp2p::peer::PeerId::fromPublicKey(marshalled);
  if (!peer_id) {
    return Error("Failed to derive PeerId from Ed25519 public key");
  }
  return peer_id.value().toBase58();
}

} // namespace pbr
