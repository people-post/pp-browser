#include "domain/relay/RelayServices.h"

#include <utility>

namespace pbr {

RelayServices::~RelayServices() {
  Stop();
}

Roe<void> RelayServices::Start(mesh::MeshHost& mesh, Options options) {
  if (started_) {
    return Error("relay services already started");
  }
  if (!mesh.Amp()) {
    return Error("relay services: the mesh is not started");
  }
  options_ = std::move(options);
  started_ = true;
  return {};
}

void RelayServices::Stop() {
  started_ = false;
}

std::vector<std::string> RelayServices::Running() const {
  return {};
}

std::optional<std::string> RelayServices::Failure() const {
  return std::nullopt;
}

} // namespace pbr
