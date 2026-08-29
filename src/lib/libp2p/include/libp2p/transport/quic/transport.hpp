/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <asio/ip/udp.hpp>
#include <libp2p/muxer/muxed_connection_config.hpp>
#include <libp2p/transport/transport_adaptor.hpp>

namespace asio {
  class io_context;
}  // namespace asio

namespace asio::ssl {
  class context;
}  // namespace asio::ssl

namespace libp2p::crypto::marshaller {
  class KeyMarshaller;
}  // namespace libp2p::crypto::marshaller

namespace libp2p::peer {
  struct IdentityManager;
}  // namespace libp2p::peer

namespace libp2p::security {
  struct SslContext;
}  // namespace libp2p::security

namespace libp2p::transport::lsquic {
  class Engine;
}  // namespace libp2p::transport::lsquic

namespace libp2p::transport {
  class QuicTransport : public TransportAdaptor,
                        public std::enable_shared_from_this<QuicTransport> {
   public:
    QuicTransport(std::shared_ptr<asio::io_context> io_context,
                  const security::SslContext &ssl_context,
                  const muxer::MuxedConnectionConfig &mux_config,
                  const peer::IdentityManager &id_mgr,
                  std::shared_ptr<crypto::marshaller::KeyMarshaller> key_codec);

    // Adaptor
    peer::ProtocolName getProtocolId() const override;

    // TransportAdaptor
    void dial(const PeerId &peer,
              Multiaddress address,
              TransportAdaptor::HandlerFunc cb) override;
    std::shared_ptr<TransportListener> createListener(
        TransportListener::HandlerFunc cb) override;
    bool canDial(const Multiaddress &ma) const override;

   private:
    std::shared_ptr<lsquic::Engine> makeClient(
        asio::ip::udp protocol) const;

    std::shared_ptr<lsquic::Engine> clientFor(
        asio::ip::udp::endpoint remote);

    std::shared_ptr<asio::io_context> io_context_;
    std::shared_ptr<asio::ssl::context> ssl_context_;
    muxer::MuxedConnectionConfig mux_config_;
    PeerId local_peer_;
    std::shared_ptr<crypto::marshaller::KeyMarshaller> key_codec_;
    asio::ip::udp::resolver resolver_;
    std::shared_ptr<lsquic::Engine> client4_, client6_;
  };
}  // namespace libp2p::transport
