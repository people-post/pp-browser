/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <libp2p/common/literals.hpp>
#include <libp2p/host/explicit_host.hpp>
#include <libp2p/log/configurator.hpp>
#include <libp2p/log/logger.hpp>
#include <libp2p/muxer/muxed_connection_config.hpp>
#include <libp2p/protocol/echo.hpp>

namespace {
  const std::string logger_config(R"(
# ----------------
sinks:
 - name: console
   type: console
   color: true
groups:
 - name: main
   sink: console
   level: info
   children:
     - name: libp2p
# ----------------
 )");
}  // namespace

int main(int argc, char **argv) {
  using libp2p::crypto::Key;
  using libp2p::crypto::KeyPair;
  using libp2p::crypto::PrivateKey;
  using libp2p::crypto::PublicKey;
  using libp2p::common::operator""_unhex;

  auto has_arg = [&](std::string_view arg) {
    auto args = std::span(argv, argc).subspan(1);
    return std::find(args.begin(), args.end(), arg) != args.end();
  };

  if (has_arg("-h") or has_arg("--help")) {
    fmt::print("Options:\n");
    fmt::print("  -h, --help\n");
    fmt::print("    Print help\n");
    fmt::print("  -insecure\n");
    fmt::print("    Use plaintext protocol instead of noise\n");
    return 0;
  }

  // prepare log system
  auto logging_system = std::make_shared<soralog::LoggingSystem>(
      std::make_shared<soralog::ConfiguratorFromYAML>(
          // Original LibP2P logging config
          std::make_shared<libp2p::log::Configurator>(),
          // Additional logging config for application
          logger_config));
  auto r = logging_system->configure();
  if (not r.message.empty()) {
    (r.has_error ? std::cerr : std::cout) << r.message << std::endl;
  }
  if (r.has_error) {
    exit(EXIT_FAILURE);
  }

  libp2p::log::setLoggingSystem(logging_system);
  if (std::getenv("TRACE_DEBUG") != nullptr) {
    libp2p::log::setLevelOfGroup("main", soralog::Level::TRACE);
  } else {
    libp2p::log::setLevelOfGroup("main", soralog::Level::INFO);
  }

  auto log = libp2p::log::createLogger("EchoServer");

  // resulting PeerId should be
  // 12D3KooWEgUjBV5FJAuBSoNMRYFRHjV7PjZwRQ7b43EKX9g7D6xV
  KeyPair keypair{PublicKey{{Key::Type::Ed25519,
                             "48453469c62f4885373099421a7365520b5ffb"
                             "0d93726c124166be4b81d852e6"_unhex}},
                  PrivateKey{{Key::Type::Ed25519,
                              "4a9361c525840f7086b893d584ebbe475b4ec"
                              "7069951d2e897e8bceb0a3f35ce"_unhex}}};

  bool insecure_mode{has_arg("-insecure")};
  if (insecure_mode) {
    log->info("Starting in insecure mode");
  } else {
    log->info("Starting in secure mode");
  }

  auto io = std::make_shared<asio::io_context>();
  const auto security = insecure_mode ? libp2p::HostSecurityKind::Plaintext
                                      : libp2p::HostSecurityKind::Noise;
  auto host = libp2p::createExplicitHost(
      io, libp2p::HostMuxerKind::Yamux, security, keypair);

  // set a handler for Echo protocol
  libp2p::protocol::Echo echo{libp2p::protocol::EchoConfig{
      .max_server_repeats =
          libp2p::protocol::EchoConfig::kInfiniteNumberOfRepeats,
      .max_recv_size =
          libp2p::muxer::MuxedConnectionConfig::kDefaultMaxWindowSize}};
  host->setProtocolHandler({echo.getProtocolId()},
                           [&echo](libp2p::StreamAndProtocol stream) {
                             echo.handle(std::move(stream));
                           });

  std::string _ma = "/ip4/127.0.0.1/tcp/40010";
  auto ma = libp2p::multi::Multiaddress::create(_ma).value();

  // launch a Listener part of the Host
  asio::post(*io, [&] {
    auto listen_res = host->listen(ma);
    if (!listen_res) {
      log->error("host cannot listen the given multiaddress: {}",
                 listen_res.error());
      std::exit(EXIT_FAILURE);
    }

    host->start();
    log->info("Server started");
    log->info("Listening on: {}", ma.getStringAddress());
    log->info("Peer id: {}", host->getPeerInfo().id.toBase58());
    log->info("Connection string: {}/p2p/{}",
              ma.getStringAddress(),
              host->getPeerInfo().id.toBase58());
  });

  // run the IO context
  try {
    io->run();
    std::exit(EXIT_SUCCESS);
  } catch (const std::error_code &ec) {
    log->error("Server cannot run: {}", ec);
    std::exit(EXIT_FAILURE);
  } catch (...) {
    log->error("Unknown error happened");
    std::exit(EXIT_FAILURE);
  }
}
