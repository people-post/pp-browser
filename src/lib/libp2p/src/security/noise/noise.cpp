/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cassert>
#include <libp2p/security/noise/handshake.hpp>
#include <libp2p/security/noise/handshake_message_marshaller_impl.hpp>
#include <libp2p/security/noise/noise.hpp>

namespace libp2p::security {
  peer::ProtocolName Noise::getProtocolId() const {
    return kProtocolId;
  }

  Noise::Noise(
      std::shared_ptr<peer::IdentityManager> idmgr,
      std::shared_ptr<crypto::CryptoProvider> crypto_provider,
      std::shared_ptr<crypto::marshaller::KeyMarshaller> key_marshaller)
      : crypto_provider_{std::move(crypto_provider)},
        key_marshaller_{std::move(key_marshaller)} {
    assert(idmgr != nullptr);
    // Copy from IdentityManager instead of taking KeyPair from DI. Boost.DI
    // moves by-value bindings; KeyPair must only be consumed once (by
    // IdentityManagerImpl) or MSVC leaves a moved-from Multihash in PeerId.
    local_key_ = idmgr->getKeyPair();
  }

  void Noise::secureInbound(
      std::shared_ptr<connection::LayerConnection> inbound,
      SecurityAdaptor::SecConnCallbackFunc cb) {
    log_->info("securing inbound connection");
    auto noise_marshaller =
        std::make_unique<noise::HandshakeMessageMarshallerImpl>(
            key_marshaller_);
    auto handshake =
        std::make_shared<noise::Handshake>(crypto_provider_,
                                           std::move(noise_marshaller),
                                           local_key_,
                                           inbound,
                                           false,
                                           std::nullopt,
                                           std::move(cb),
                                           key_marshaller_);
    handshake->connect();
  }

  void Noise::secureOutbound(
      std::shared_ptr<connection::LayerConnection> outbound,
      const peer::PeerId &p,
      SecurityAdaptor::SecConnCallbackFunc cb) {
    log_->info("securing outbound connection");
    auto noise_marshaller =
        std::make_unique<noise::HandshakeMessageMarshallerImpl>(
            key_marshaller_);
    auto handshake =
        std::make_shared<noise::Handshake>(crypto_provider_,
                                           std::move(noise_marshaller),
                                           local_key_,
                                           outbound,
                                           true,
                                           p,
                                           std::move(cb),
                                           key_marshaller_);
    handshake->connect();
  }
}  // namespace libp2p::security
