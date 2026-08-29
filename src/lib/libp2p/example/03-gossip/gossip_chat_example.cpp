/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <libp2p/injector/host_injector.hpp>
#include <libp2p/log/configurator.hpp>
#include <libp2p/protocol/gossip/gossip.hpp>

#include "console_async_reader.hpp"
#include "utility.hpp"

namespace {
  // cmd line options
  struct Options {
    // local node port
    int port = 0;

    // topic name
    std::string topic = "chat";

    // optional remote peer to connect to
    std::optional<libp2p::peer::PeerInfo> remote;

    // log level: 'd' for debug, 'i' for info, 'w' for warning, 'e' for error
    char log_level = 'w';
  };

  // parses command line, returns non-empty Options on success
  std::optional<Options> parseCommandLine(int argc, char **argv);

  void printUsage();

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

int main(int argc, char *argv[]) {
  namespace utility = libp2p::protocol::example::utility;

  auto options = parseCommandLine(argc, argv);
  if (!options) {
    return 1;
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
    libp2p::log::setLevelOfGroup("main", soralog::kLevelError);
  }

  // overriding default config to see local messages as well (echo mode)
  libp2p::protocol::gossip::Config config;
  config.echo_forward_mode = true;

  // injector creates and ties dependent objects
  auto injector = libp2p::injector::makeHostInjector();

  utility::setupLoggers(options->log_level);

  // create asio context
  auto io = injector.create<std::shared_ptr<boost::asio::io_context>>();

  // host is our local libp2p node
  auto host = injector.create<std::shared_ptr<libp2p::Host>>();

  // make peer uri of local node
  auto local_address_str = fmt::format("/ip4/{}/tcp/{}/p2p/{}",
                                       utility::getLocalIP(*io),
                                       options->port,
                                       host->getId().toBase58());

  // local address -> peer info
  auto peer_info = utility::str2peerInfo(local_address_str);
  if (!peer_info) {
    std::cerr << "Cannot resolve local peer from " << local_address_str << "\n";
    return 2;
  }

  // say local address
  std::cerr << "I am " << local_address_str << "\n";

  // create gossip node
  auto gossip = libp2p::protocol::gossip::create(
      injector.create<std::shared_ptr<libp2p::basic::Scheduler>>(),
      host,
      injector.create<std::shared_ptr<libp2p::peer::IdentityManager>>(),
      injector.create<std::shared_ptr<libp2p::crypto::CryptoProvider>>(),
      injector
          .create<std::shared_ptr<libp2p::crypto::marshaller::KeyMarshaller>>(),
      std::move(config));

  using Message = libp2p::protocol::gossip::Gossip::Message;

  // subscribe to chat topic, print messages to the console
  auto subscription = gossip->subscribe(
      {options->topic}, [](const Message *m) {
        if (!m) {
          // nullptr means EOS, this occurs when the node has stopped
          return;
        }
        std::cerr << utility::formatPeerId(m->from) << ": "
                  << utility::toString(m->data) << "\n";
      });

  // tell gossip to connect to remote peer, only if specified
  if (options->remote) {
    gossip->addBootstrapPeer(options->remote->id,
                             options->remote->addresses[0]);
  }

  // start the node as soon as async engine starts
  post(*io, [&] {
    auto listen_res = host->listen(peer_info->addresses[0]);
    if (!listen_res) {
      fmt::println(std::cerr,
                   "Cannot listen to multiaddress {}, {}",
                   peer_info->addresses[0].getStringAddress(),
                   listen_res.error());
      io->stop();
      return;
    }
    host->start();
    gossip->start();
    std::cerr << "Node started\n";
  });

  // read lines from stdin in async manner and publish them into the chat
  utility::ConsoleAsyncReader stdin_reader(
      *io, [&gossip, &options](const std::string &msg) {
        gossip->publish({options->topic}, utility::fromString(msg));
      });

  // gracefully shutdown on signal
  boost::asio::signal_set signals(*io, SIGINT, SIGTERM);
  signals.async_wait(
      [&io](const boost::system::error_code &, int) { io->stop(); });

  // run event loop
  io->run();
  std::cerr << "Node stopped\n";

  return 0;
}

namespace {

  void printUsage() {
    std::cerr
        << "gossip_chat_example options:\n"
           "  -h, --help              print usage message\n"
           "  -p, --port <port>       port to listen to\n"
           "  -t, --topic <name>      chat topic name (default is 'chat')\n"
           "  -r, --remote <uri>      remote peer uri to connect to\n"
           "  -l, --log <e|w|i|d>     log level\n";
  }

  std::optional<std::string_view> takeArg(int &i, int argc, char **argv) {
    if (i + 1 >= argc) {
      return std::nullopt;
    }
    ++i;
    return std::string_view{argv[i]};
  }

  std::optional<Options> parseCommandLine(int argc, char **argv) {
    try {
      Options o;
      std::string remote;

      if (argc <= 1) {
        printUsage();
        return std::nullopt;
      }

      for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "-h" || arg == "--help") {
          printUsage();
          return std::nullopt;
        }
        if (arg == "-p" || arg == "--port") {
          auto v = takeArg(i, argc, argv);
          if (!v) {
            std::cerr << "Missing value for " << arg << "\n";
            return std::nullopt;
          }
          o.port = std::stoi(std::string{*v});
        } else if (arg == "-t" || arg == "--topic") {
          auto v = takeArg(i, argc, argv);
          if (!v) {
            std::cerr << "Missing value for " << arg << "\n";
            return std::nullopt;
          }
          o.topic = std::string{*v};
        } else if (arg == "-r" || arg == "--remote") {
          auto v = takeArg(i, argc, argv);
          if (!v) {
            std::cerr << "Missing value for " << arg << "\n";
            return std::nullopt;
          }
          remote = std::string{*v};
        } else if (arg == "-l" || arg == "--log") {
          auto v = takeArg(i, argc, argv);
          if (!v || v->empty()) {
            std::cerr << "Missing value for " << arg << "\n";
            return std::nullopt;
          }
          o.log_level = (*v)[0];
        } else {
          std::cerr << "Unknown option: " << arg << "\n";
          printUsage();
          return std::nullopt;
        }
      }

      if (o.port == 0) {
        std::cerr << "Port cannot be zero\n";
        return std::nullopt;
      }

      if (o.topic.empty()) {
        std::cerr << "Topic name cannot be empty\n";
        return std::nullopt;
      }

      if (!remote.empty()) {
        o.remote = libp2p::protocol::example::utility::str2peerInfo(remote);
        if (!o.remote) {
          std::cerr << "Cannot resolve remote peer address from " << remote
                    << "\n";
          return std::nullopt;
        }
      }

      return o;

    } catch (const std::exception &e) {
      std::cerr << e.what() << "\n";
    }
    return std::nullopt;
  }

}  // namespace
