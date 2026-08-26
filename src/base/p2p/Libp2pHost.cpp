#include "base/p2p/Libp2pHost.h"

#include "base/runtime/WorkerDispatch.h"

#include <libp2p/crypto/key.hpp>
#include <libp2p/host/explicit_host.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/log/configurator.hpp>
#include <libp2p/log/logger.hpp>
#include <libp2p/multi/multiaddress.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <condition_variable>
#include <cstdlib>
#include <cassert>
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
  if (!config.device_ml_dsa_private_key || !config.device_ml_dsa_public_key) {
    return std::nullopt;
  }
  if (config.device_ml_dsa_private_key->size() != 4032
      || config.device_ml_dsa_public_key->size() != 1952) {
    return std::nullopt;
  }
  libp2p::crypto::KeyPair pair;
  pair.privateKey.type = libp2p::crypto::Key::Type::MlDsa65;
  pair.privateKey.data = *config.device_ml_dsa_private_key;
  pair.publicKey.type = libp2p::crypto::Key::Type::MlDsa65;
  pair.publicKey.data = *config.device_ml_dsa_public_key;
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
  if (WorkerDispatch::IsInstalled()) {
    owned_worker_pool_.reset();
    worker_pool_ = nullptr;
  } else {
    owned_worker_pool_ = std::make_unique<WorkerPool>();
    worker_pool_ = owned_worker_pool_.get();
  }

  io_context_ = std::make_shared<boost::asio::io_context>(1);
  // Keep run() alive after the startup post: an idle Client host otherwise drains
  // immediately and Identify PostAndWait hangs forever.
  work_guard_.emplace(boost::asio::make_work_guard(*io_context_));
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
    owned_worker_pool_.reset();
    worker_pool_ = nullptr;
    work_guard_.reset();
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
    if (owned_worker_pool_) {
      owned_worker_pool_->Shutdown();
      owned_worker_pool_.reset();
    }
    worker_pool_ = nullptr;
    return;
  }
  if (owned_worker_pool_) {
    owned_worker_pool_->Shutdown();
    owned_worker_pool_.reset();
  }
  work_guard_.reset();
  if (io_context_) {
    io_context_->stop();
  }
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  host_.reset();
  io_context_.reset();
  worker_pool_ = nullptr;
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
  // Prefer interface (actual bound) addresses over listen-request keys so tcp/0
  // resolves to the OS-assigned port for mDNS / Identify publish.
  for (const auto& ma : host_->getAddressesInterfaces()) {
    out.emplace_back(ma.getStringAddress());
  }
  if (out.empty()) {
    for (const auto& ma : host_->getAddresses()) {
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

boost::asio::any_io_executor Libp2pHost::IoExecutor() const {
  if (!io_context_) {
    return boost::asio::any_io_executor{};
  }
  return io_context_->get_executor();
}

WorkerPool& Libp2pHost::GetWorkerPool() {
  assert(worker_pool_ != nullptr && "Libp2pHost::GetWorkerPool requires a private test pool");
  return *worker_pool_;
}

const WorkerPool& Libp2pHost::GetWorkerPool() const {
  return const_cast<Libp2pHost*>(this)->GetWorkerPool();
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

Roe<void> Libp2pHost::ListenOn(const std::string& multiaddr_str) {
  if (!running_ || !host_ || !io_context_) {
    return Error("libp2p host not running");
  }
  auto ma_res = libp2p::multi::Multiaddress::create(multiaddr_str);
  if (!ma_res) {
    return Error("invalid libp2p listen multiaddr: " + multiaddr_str);
  }
  const libp2p::multi::Multiaddress ma = ma_res.value();

  std::promise<Roe<void>> listen_promise;
  auto listen_future = listen_promise.get_future();
  boost::asio::post(*io_context_, [this, ma, addr = multiaddr_str, listen_promise = std::move(listen_promise)]() mutable {
    listen_promise.set_value(ListenOnIoThread(ma, addr));
  });
  return listen_future.get();
}

void Libp2pHost::ListenOnAsync(const std::string& multiaddr_str, std::function<void(Roe<void>)> cb) {
  if (!cb) {
    return;
  }
  if (!running_ || !host_ || !io_context_) {
    cb(Error("libp2p host not running"));
    return;
  }
  auto ma_res = libp2p::multi::Multiaddress::create(multiaddr_str);
  if (!ma_res) {
    cb(Error("invalid libp2p listen multiaddr: " + multiaddr_str));
    return;
  }
  const libp2p::multi::Multiaddress ma = ma_res.value();
  boost::asio::post(*io_context_, [this, ma, addr = multiaddr_str, cb = std::move(cb)]() mutable {
    cb(ListenOnIoThread(ma, addr));
  });
}

Roe<void> Libp2pHost::ListenOnIoThread(const libp2p::multi::Multiaddress& ma, const std::string& addr) {
  auto listen_res = host_->listen(ma);
  if (!listen_res) {
    return Error("libp2p listen failed on " + addr);
  }
  // Confirm an OS-assigned port exists when the request used tcp/0 (N025).
  bool have_port = false;
  for (const auto& bound : host_->getAddressesInterfaces()) {
    const std::string s(bound.getStringAddress());
    const auto pos = s.find("/tcp/");
    if (pos == std::string::npos) {
      continue;
    }
    const long port = std::strtol(s.c_str() + static_cast<std::ptrdiff_t>(pos + 5), nullptr, 10);
    if (port > 0) {
      have_port = true;
      break;
    }
  }
  if (!have_port) {
    return Error("libp2p listen produced no bound tcp port for " + addr);
  }
  return {};
}

Roe<void> Libp2pHost::StopListening() {
  // Closes every listener on the host — used for mobile Client ephemeral bind (N025) only.
  if (!running_ || !host_ || !io_context_) {
    return {};
  }
  std::promise<Roe<void>> done_promise;
  auto done_future = done_promise.get_future();
  boost::asio::post(*io_context_, [this, done_promise = std::move(done_promise)]() mutable {
    StopListeningIoThread();
    done_promise.set_value({});
  });
  return done_future.get();
}

void Libp2pHost::StopListeningAsync(std::function<void()> cb) {
  if (!running_ || !host_ || !io_context_) {
    if (cb) {
      cb();
    }
    return;
  }
  boost::asio::post(*io_context_, [this, cb = std::move(cb)]() mutable {
    StopListeningIoThread();
    if (cb) {
      cb();
    }
  });
}

void Libp2pHost::StopListeningIoThread() {
  if (!host_) {
    return;
  }
  const auto addrs = host_->getAddresses();
  for (const auto& ma : addrs) {
    (void)host_->closeListener(ma);
    (void)host_->removeListener(ma);
  }
}

} // namespace pbr
