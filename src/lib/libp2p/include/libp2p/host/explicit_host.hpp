/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <optional>

#include <asio/io_context.hpp>

#include <libp2p/crypto/key.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/muxer/muxed_connection_config.hpp>

namespace libp2p {

  enum class HostMuxerKind { Yamux, Mplex };

  enum class HostSecurityKind { Plaintext, Noise, Tls };

  enum class HostTransportKind { Tcp, Quic };

  /**
   * Build a Host with explicit shared_ptr wiring (no Boost.DI).
   *
   * Preferred Host construction path for pp-browser and regression tests.
   * IdentityManagerImpl is the sole consumer of the KeyPair; security
   * adaptors copy keys from IdentityManager.
   *
   * @param io shared io_context used by transport, scheduler, and DNS resolver
   * @param muxer stream muxer adaptor (TCP path; Quic muxes in-transport)
   * @param security security adaptor (TCP path; Quic uses TLS via SslContext)
   * @param key_pair optional identity; generates ML-DSA-65 (Noise) or Ed25519
   * @param mux_config yamux/mplex (and Quic) connection config
   * @param transport Tcp (default) or Quic (/udp/.../quic-v1)
   */
  std::shared_ptr<Host> createExplicitHost(
      std::shared_ptr<asio::io_context> io,
      HostMuxerKind muxer = HostMuxerKind::Yamux,
      HostSecurityKind security = HostSecurityKind::Noise,
      std::optional<crypto::KeyPair> key_pair = std::nullopt,
      muxer::MuxedConnectionConfig mux_config = {},
      HostTransportKind transport = HostTransportKind::Tcp);

}  // namespace libp2p
