#pragma once

#include "common/Error.h"

#include <string>
#include <vector>

namespace pbr {

/** Derive libp2p PeerId (base58) from a raw ML-DSA-65 public key (1952 bytes). */
Roe<std::string> PeerIdFromMlDsaPublicKey(const std::vector<uint8_t>& public_key);

} // namespace pbr
