#include "domain/relay/RelayService.h"

#include <utility>

namespace pbr::relay {

RelayService::~RelayService() {
  Stop();
}

Roe<void> RelayService::Start(mesh::MeshHost& mesh, Options options) {
  if (started_) {
    return Error("relay service already started");
  }
  if (!options.settings.enabled) {
    return {};
  }
  if (!mesh.Amp()) {
    return Error("relay service: the mesh is not started");
  }
  RelayLimits limits;
  limits.retention_days = options.settings.retention_days;
  limits.max_records = options.settings.max_records;
  limits.max_bytes = options.settings.max_bytes;
  auto store = RelayStore::Open(options.work_dir, limits, options.clock);
  if (!store) {
    return store.error();
  }
  store_ = std::move(*store);
  options_ = std::move(options);
  started_ = true;
  return {};
}

void RelayService::Stop() {
  started_ = false;
  store_.reset();
}

std::optional<std::string> RelayService::Failure() const {
  return std::nullopt;
}

} // namespace pbr::relay
