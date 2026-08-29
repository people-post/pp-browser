/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/crypto/key.hpp>

#include <libp2p/common/hash.hpp>

size_t std::hash<libp2p::crypto::Key>::operator()(
    const libp2p::crypto::Key &x) const {
  size_t seed = 0;
  libp2p::common::hashCombine(seed, x.type);
  libp2p::common::hashCombine(seed,
                              libp2p::common::hashRange(x.data.begin(), x.data.end()));
  return seed;
}

size_t std::hash<libp2p::crypto::PrivateKey>::operator()(
    const libp2p::crypto::PrivateKey &x) const {
  return std::hash<libp2p::crypto::Key>()(x);
}

size_t std::hash<libp2p::crypto::PublicKey>::operator()(
    const libp2p::crypto::PublicKey &x) const {
  return std::hash<libp2p::crypto::Key>()(x);
}

size_t std::hash<libp2p::crypto::KeyPair>::operator()(
    const libp2p::crypto::KeyPair &x) const {
  using libp2p::crypto::PrivateKey;
  using libp2p::crypto::PublicKey;
  size_t seed = 0;
  libp2p::common::hashCombine(seed, std::hash<PublicKey>()(x.publicKey));
  libp2p::common::hashCombine(seed, std::hash<PrivateKey>()(x.privateKey));
  return seed;
}
