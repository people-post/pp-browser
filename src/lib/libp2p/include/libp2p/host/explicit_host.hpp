/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <optional>

#include <boost/asio/io_context.hpp>

#include <libp2p/crypto/key.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/muxer/muxed_connection_config.hpp>

namespace libp2p {

  enum class HostMuxerKind { Yamux, Mplex };

  enum class HostSecurityKind { Plaintext, Noise, Tls };

  /**
   * Build a TCP Host with explicit shared_ptr wiring (no Boost.DI).
   *
   * Preferred Host construction path for pp-browser and regression tests.
   * IdentityManagerImpl is the sole consumer of the KeyPair; security
   * adaptors copy keys from IdentityManager.
   *
   * @param io shared io_context used by TCP, scheduler, and DNS resolver
   * @param muxer stream muxer adaptor
   * @param security security adaptor
   * @param key_pair optional identity; generates Ed25519 when nullopt
   * @param mux_config yamux/mplex connection config
   */
  std::shared_ptr<Host> createExplicitHost(
      std::shared_ptr<boost::asio::io_context> io,
      HostMuxerKind muxer = HostMuxerKind::Yamux,
      HostSecurityKind security = HostSecurityKind::Noise,
      std::optional<crypto::KeyPair> key_pair = std::nullopt,
      muxer::MuxedConnectionConfig mux_config = {});

}  // namespace libp2p
