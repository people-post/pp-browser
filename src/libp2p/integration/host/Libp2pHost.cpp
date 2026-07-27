#include "libp2p/integration/host/Libp2pHost.h"

#include <libp2p/crypto/key.hpp>
#include <libp2p/host/explicit_host.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/log/configurator.hpp>
#include <libp2p/log/logger.hpp>
#include <libp2p/multi/multiaddress.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <condition_variable>
#include <future>
#include <mutex>

#include <soralog/impl/configurator_from_yaml.hpp>

namespace pbr {

namespace {

void EnsureLibp2pLoggingInitialized() {
  static std::once_flag once;
  std::call_once(once, [] {
    static const std::string kLibp2pLogConfig = R"(
sinks:
  - name: console
    type: console
    color: false
groups:
  - name: libp2p
    sink: console
    level: error
)";
    auto logging_system = std::make_shared<soralog::LoggingSystem>(
        std::make_shared<soralog::ConfiguratorFromYAML>(std::make_shared<libp2p::log::Configurator>(),
                                                        kLibp2pLogConfig));
    const auto result = logging_system->configure();
    if (result.has_error) {
      return;
    }
    libp2p::log::setLoggingSystem(logging_system);
  });
}

std::optional<libp2p::crypto::KeyPair> BuildKeyPair(const Libp2pHostConfig& config) {
  if (!config.ed25519_private_key || !config.ed25519_public_key) {
    return std::nullopt;
  }
  if (config.ed25519_private_key->size() != 32 || config.ed25519_public_key->size() != 32) {
    return std::nullopt;
  }
  libp2p::crypto::KeyPair pair;
  pair.privateKey.type = libp2p::crypto::Key::Type::Ed25519;
  pair.privateKey.data = *config.ed25519_private_key;
  pair.publicKey.type = libp2p::crypto::Key::Type::Ed25519;
  pair.publicKey.data = *config.ed25519_public_key;
  return pair;
}

} // namespace

Libp2pHost::Libp2pHost() {
  available_ = true;
}

Libp2pHost::~Libp2pHost() {
  Stop();
}

void Libp2pHost::EnsureLogging() {
  EnsureLibp2pLoggingInitialized();
}

Roe<void> Libp2pHost::Start(const Libp2pHostConfig& config) {
  if (running_.exchange(true)) {
    return {};
  }

  EnsureLogging();
  config_ = config;

  io_context_ = std::make_shared<boost::asio::io_context>(1);
  auto key_pair = BuildKeyPair(config_);
  host_ = libp2p::createExplicitHost(io_context_, libp2p::HostMuxerKind::Yamux, libp2p::HostSecurityKind::Noise,
                                     key_pair);

  std::promise<Roe<void>> listen_promise;
  auto listen_future = listen_promise.get_future();

  if (!config_.listen_enabled) {
    io_thread_ = std::thread([this, listen_promise = std::move(listen_promise)]() mutable {
      boost::asio::post(*io_context_, [this, listen_promise = std::move(listen_promise)]() mutable {
        host_->start();
        listen_promise.set_value({});
      });
      io_context_->run();
    });
    auto start_result = listen_future.get();
    if (!start_result) {
      Stop();
      return start_result.error();
    }
    return {};
  }

  auto ma_res = libp2p::multi::Multiaddress::create(config_.listen_multiaddr);
  if (!ma_res) {
    running_ = false;
    host_.reset();
    io_context_.reset();
    return Error("Invalid libp2p listen multiaddr: " + config_.listen_multiaddr);
  }
  const libp2p::multi::Multiaddress ma = ma_res.value();

  io_thread_ = std::thread([this, ma, listen_promise = std::move(listen_promise)]() mutable {
    boost::asio::post(*io_context_, [this, ma, listen_promise = std::move(listen_promise)]() mutable {
      auto listen_res = host_->listen(ma);
      if (!listen_res) {
        listen_promise.set_value(Error("libp2p listen failed on " + config_.listen_multiaddr));
        return;
      }
      host_->start();
      listen_promise.set_value({});
    });
    io_context_->run();
  });

  auto listen_result = listen_future.get();
  if (!listen_result) {
    Stop();
    return listen_result.error();
  }
  return {};
}

void Libp2pHost::Stop() {
  if (!running_.exchange(false)) {
    if (io_thread_.joinable()) {
      io_thread_.join();
    }
    return;
  }
  if (io_context_) {
    io_context_->stop();
  }
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  host_.reset();
  io_context_.reset();
}

libp2p::Host& Libp2pHost::GetHost() {
  return *host_;
}

const libp2p::Host& Libp2pHost::GetHost() const {
  return *host_;
}

std::shared_ptr<libp2p::Host> Libp2pHost::SharedHost() const {
  return host_;
}

Roe<std::string> Libp2pHost::LocalPeerIdBase58() const {
  if (!host_) {
    return Error("libp2p host not started");
  }
  return host_->getId().toBase58();
}

std::vector<std::string> Libp2pHost::ListenMultiaddrs() const {
  std::vector<std::string> out;
  if (!host_) {
    return out;
  }
  for (const auto& ma : host_->getAddresses()) {
    out.emplace_back(ma.getStringAddress());
  }
  if (out.empty()) {
    for (const auto& ma : host_->getAddressesInterfaces()) {
      out.emplace_back(ma.getStringAddress());
    }
  }
  return out;
}

void Libp2pHost::Post(std::function<void()> fn) {
  if (!running_ || !io_context_ || !fn) {
    return;
  }
  boost::asio::post(*io_context_, std::move(fn));
}

Roe<void> Libp2pHost::PostAndWait(std::function<void()> fn) {
  if (!running_ || !io_context_) {
    return Error("libp2p host not running");
  }
  if (!fn) {
    return {};
  }
  std::promise<void> done;
  auto future = done.get_future();
  boost::asio::post(*io_context_, [fn = std::move(fn), done = std::move(done)]() mutable {
    fn();
    done.set_value();
  });
  future.get();
  return {};
}

} // namespace pbr
