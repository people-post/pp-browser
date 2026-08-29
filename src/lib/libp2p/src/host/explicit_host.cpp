/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/host/explicit_host.hpp>

#include <libp2p/basic/scheduler/asio_scheduler_backend.hpp>
#include <libp2p/basic/scheduler/scheduler_impl.hpp>
#include <libp2p/crypto/crypto_provider/crypto_provider_impl.hpp>
#include <libp2p/crypto/ecdsa_provider/ecdsa_provider_impl.hpp>
#include <libp2p/crypto/ed25519_provider/ed25519_provider_impl.hpp>
#include <libp2p/crypto/hmac_provider/hmac_provider_impl.hpp>
#include <libp2p/crypto/key_marshaller/key_marshaller_impl.hpp>
#include <libp2p/crypto/key_validator/key_validator_impl.hpp>
#include <libp2p/crypto/mldsa_provider/mldsa_provider_impl.hpp>
#include <libp2p/crypto/random_generator/std_generator.hpp>
#include <libp2p/crypto/rsa_provider/rsa_provider_impl.hpp>
#include <libp2p/crypto/secp256k1_provider/secp256k1_provider_impl.hpp>
#include <libp2p/event/bus.hpp>
#include <libp2p/host/basic_host/basic_host.hpp>
#include <libp2p/muxer/mplex.hpp>
#include <libp2p/muxer/yamux.hpp>
#include <libp2p/network/cares/cares.hpp>
#include <libp2p/network/impl/connection_manager_impl.hpp>
#include <libp2p/network/impl/dialer_impl.hpp>
#include <libp2p/network/impl/dnsaddr_resolver_impl.hpp>
#include <libp2p/network/impl/listener_manager_impl.hpp>
#include <libp2p/network/impl/network_impl.hpp>
#include <libp2p/network/impl/router_impl.hpp>
#include <libp2p/network/impl/transport_manager_impl.hpp>
#include <libp2p/peer/address_repository/inmem_address_repository.hpp>
#include <libp2p/peer/impl/identity_manager_impl.hpp>
#include <libp2p/peer/impl/peer_repository_impl.hpp>
#include <libp2p/peer/key_repository/inmem_key_repository.hpp>
#include <libp2p/peer/protocol_repository/inmem_protocol_repository.hpp>
#include <libp2p/protocol_muxer/multiselect.hpp>
#include <libp2p/security/noise.hpp>
#include <libp2p/security/plaintext.hpp>
#include <libp2p/security/plaintext/exchange_message_marshaller_impl.hpp>
#include <libp2p/security/tls/ssl_context.hpp>
#include <libp2p/security/tls/tls_adaptor.hpp>
#include <libp2p/transport/impl/upgrader_impl.hpp>
#include <libp2p/transport/tcp/tcp_transport.hpp>

namespace libp2p {
  namespace {
    // DnsaddrResolverImpl holds a non-owning const Ares&; keep process-lifetime.
    network::c_ares::Ares &caresInstance() {
      static network::c_ares::Ares cares;
      return cares;
    }
  }  // namespace

  std::shared_ptr<Host> createExplicitHost(
      std::shared_ptr<boost::asio::io_context> io,
      HostMuxerKind muxer_kind,
      HostSecurityKind security_kind,
      std::optional<crypto::KeyPair> key_pair,
      muxer::MuxedConnectionConfig mux_config) {
    auto csprng = std::make_shared<crypto::random::StdRandomGenerator>();
    auto ed25519 =
        std::make_shared<crypto::ed25519::Ed25519ProviderImpl>();
    auto rsa = std::make_shared<crypto::rsa::RsaProviderImpl>();
    auto ecdsa = std::make_shared<crypto::ecdsa::EcdsaProviderImpl>();
    auto secp256k1 =
        std::make_shared<crypto::secp256k1::Secp256k1ProviderImpl>(csprng);
    auto hmac = std::make_shared<crypto::hmac::HmacProviderImpl>();
    auto mldsa = std::make_shared<crypto::mldsa::MlDsaProviderImpl>();

    std::shared_ptr<crypto::CryptoProvider> crypto_provider =
        std::make_shared<crypto::CryptoProviderImpl>(
            csprng, ed25519, rsa, ecdsa, secp256k1, hmac, mldsa);

    if (!key_pair) {
      // Product Noise path uses ML-DSA-65. TLS adaptor still hard-codes
      // Ed25519 libp2p identity extensions (marshalled pk size 36).
      const auto default_type = security_kind == HostSecurityKind::Noise
                                    ? crypto::Key::Type::MlDsa65
                                    : crypto::Key::Type::Ed25519;
      key_pair = crypto_provider->generateKeys(default_type).value();
    }

    auto key_validator =
        std::make_shared<crypto::validator::KeyValidatorImpl>(crypto_provider);
    auto key_marshaller =
        std::make_shared<crypto::marshaller::KeyMarshallerImpl>(key_validator);

    // IdentityManagerImpl is the sole consumer of the KeyPair (by value).
    auto idmgr = std::make_shared<peer::IdentityManagerImpl>(
        std::move(*key_pair), key_marshaller);

    auto scheduler_backend =
        std::make_shared<basic::AsioSchedulerBackend>(io);
    auto scheduler = std::make_shared<basic::SchedulerImpl>(
        scheduler_backend, basic::Scheduler::Config{});

    auto multiselect =
        std::make_shared<protocol_muxer::multiselect::Multiselect>(scheduler);

    auto bus = std::make_shared<event::Bus>();
    auto cmgr = std::make_shared<network::ConnectionManagerImpl>(bus);
    auto router = std::make_shared<network::RouterImpl>();

    std::vector<std::shared_ptr<security::SecurityAdaptor>> security_adaptors;
    switch (security_kind) {
      case HostSecurityKind::Plaintext: {
        auto exchange_marshaller = std::make_shared<
            security::plaintext::ExchangeMessageMarshallerImpl>(
            key_marshaller);
        security_adaptors.emplace_back(std::make_shared<security::Plaintext>(
            std::move(exchange_marshaller), idmgr, key_marshaller));
        break;
      }
      case HostSecurityKind::Noise:
        security_adaptors.emplace_back(std::make_shared<security::Noise>(
            idmgr, crypto_provider, key_marshaller));
        break;
      case HostSecurityKind::Tls: {
        security::SslContext ssl_context{*idmgr, *key_marshaller};
        security_adaptors.emplace_back(std::make_shared<security::TlsAdaptor>(
            idmgr, io, ssl_context, key_marshaller));
        break;
      }
    }

    std::vector<std::shared_ptr<muxer::MuxerAdaptor>> muxer_adaptors;
    switch (muxer_kind) {
      case HostMuxerKind::Yamux:
        muxer_adaptors.emplace_back(std::make_shared<muxer::Yamux>(
            mux_config, scheduler, cmgr));
        break;
      case HostMuxerKind::Mplex:
        muxer_adaptors.emplace_back(
            std::make_shared<muxer::Mplex>(mux_config));
        break;
    }

    std::vector<std::shared_ptr<layer::LayerAdaptor>> layer_adaptors;

    auto upgrader = std::make_shared<transport::UpgraderImpl>(
        multiselect,
        std::move(layer_adaptors),
        std::move(security_adaptors),
        std::move(muxer_adaptors));

    std::vector<std::shared_ptr<transport::TransportAdaptor>> transports = {
        std::make_shared<transport::TcpTransport>(
            io, mux_config, std::move(upgrader))};

    auto tmgr = std::make_shared<network::TransportManagerImpl>(
        std::move(transports));

    auto listener = std::make_shared<network::ListenerManagerImpl>(
        multiselect, std::move(router), tmgr, cmgr);

    auto dnsaddr_resolver = std::make_shared<network::DnsaddrResolverImpl>(
        io, caresInstance());
    auto addr_repo =
        std::make_shared<peer::InmemAddressRepository>(dnsaddr_resolver);

    auto dialer = std::make_shared<network::DialerImpl>(
        multiselect, tmgr, cmgr, listener, addr_repo, scheduler);

    auto network = std::make_unique<network::NetworkImpl>(
        std::move(listener), std::move(dialer), cmgr);

    auto peer_repo = std::make_unique<peer::PeerRepositoryImpl>(
        std::move(addr_repo),
        std::make_shared<peer::InmemKeyRepository>(),
        std::make_shared<peer::InmemProtocolRepository>());

    return std::make_shared<host::BasicHost>(idmgr,
                                             std::move(network),
                                             std::move(peer_repo),
                                             std::move(bus),
                                             std::move(tmgr),
                                             Libp2pClientVersion{"cpp-libp2p"});
  }

}  // namespace libp2p
