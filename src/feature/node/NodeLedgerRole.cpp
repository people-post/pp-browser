#include "feature/node/NodeLedgerRole.h"

#include "network/AmpLedgerServer.h"
#include "server/RoleRunner.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pbr {

struct NodeLedgerRole::Impl {
  std::unique_ptr<pp::RoleRunner> runner;
  std::thread thread;
  std::atomic<bool> listening{false};
  mutable std::mutex mu;
  std::optional<std::string> failure;
};

std::optional<NodeLedgerRole::Role> NodeLedgerRole::RoleNamed(const std::string& name) {
  if (name == "relay") {
    return Role::Relay;
  }
  if (name == "miner") {
    return Role::Miner;
  }
  return std::nullopt;
}

NodeLedgerRole::NodeLedgerRole() : impl_(std::make_unique<Impl>()) {}

NodeLedgerRole::~NodeLedgerRole() { Stop(); }

Roe<void> NodeLedgerRole::Start(mesh::MeshHost& mesh, Options options) {
  if (impl_->runner) {
    return Error("ledger role already started");
  }
  if (!mesh.Amp()) {
    return Error("ledger role: the mesh is not started");
  }
  std::error_code ec;
  std::filesystem::create_directories(options.work_dir, ec);
  if (ec) {
    return Error("ledger role: cannot create work directory " + options.work_dir + ": " + ec.message());
  }

  pp::RoleRunner::Options run;
  run.role = options.role == Role::Miner ? pp::RoleRunner::Role::Miner : pp::RoleRunner::Role::Relay;
  run.workDir = std::move(options.work_dir);
  run.host = pp::network::HostedAmpRuntime::Host{&mesh.Amp()->Runtime(),
                                                  [&mesh]() { return mesh.AmpListenMultiaddr(); }};
  // The mesh refuses opens for protocols nothing serves: advertise the ledger's only while bound.
  run.onListening = [&mesh, impl = impl_.get()](const bool listening) {
    impl->listening = listening;
    mesh.SetAppProtocols(listening ? pp::network::AmpLedgerServer::protocolIds() : std::vector<std::string>{});
  };
  impl_->runner = std::make_unique<pp::RoleRunner>(std::move(run));
  impl_->thread = std::thread([impl = impl_.get()]() {
    const auto ran = impl->runner->run();
    if (impl->runner->stopRequested()) {
      return;
    }
    // It runs until stopped: anything else is a failure the node reports.
    std::lock_guard lock(impl->mu);
    impl->failure = ran.isOk() ? "the ledger role ended" : ran.error().message;
  });
  return {};
}

void NodeLedgerRole::Stop() {
  if (!impl_->runner) {
    return;
  }
  impl_->runner->stop();
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  impl_->runner.reset();
}

bool NodeLedgerRole::Listening() const { return impl_->listening; }

std::optional<std::string> NodeLedgerRole::Failure() const {
  std::lock_guard lock(impl_->mu);
  return impl_->failure;
}

} // namespace pbr
