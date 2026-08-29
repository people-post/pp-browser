/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cassert>
#include <libp2p/peer/impl/identity_manager_impl.hpp>

#include <libp2p/crypto/key_marshaller.hpp>

namespace libp2p::peer {

  const peer::PeerId &IdentityManagerImpl::getId() const {
    assert(id_ != nullptr);
    return *id_;
  }

  const crypto::KeyPair &IdentityManagerImpl::getKeyPair() const {
    assert(keyPair_ != nullptr);
    return *keyPair_;
  }

  IdentityManagerImpl::IdentityManagerImpl(
      crypto::KeyPair keyPair,
      const std::shared_ptr<crypto::marshaller::KeyMarshaller> &marshaller) {
    assert(!keyPair.publicKey.data.empty());
    assert(marshaller);

    keyPair_ = std::make_unique<crypto::KeyPair>(std::move(keyPair));

    // it is ok to use .value(); copy PeerId out of the outcome (no move-from).
    auto marshalled_key = marshaller->marshal(keyPair_->publicKey);
    auto peer_id_res = peer::PeerId::fromPublicKey(marshalled_key.value());
    id_ = std::make_unique<peer::PeerId>(peer_id_res.value());
  }
}  // namespace libp2p::peer
