#pragma once

#include "common/Error.h"

#include <string>
#include <vector>

namespace pbr {

/** Derive libp2p PeerId (base58) from a raw Ed25519 public key (32 bytes). */
Roe<std::string> PeerIdFromEd25519PublicKey(const std::vector<uint8_t>& public_key);

} // namespace pbr
