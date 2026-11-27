#include "domain/relay/RelayService.h"

#include <utility>

namespace pbr::plus {

RelayService::~RelayService() {
  Stop();
}

Roe<void> RelayService::Start(mesh::MeshHost& mesh, Options options) {
  if (started_) {
    return Error("relay service already started");
  }
  if (!mesh.Amp()) {
    return Error("relay service: the mesh is not started");
  }
  options_ = std::move(options);
  started_ = true;
  return {};
}

void RelayService::Stop() {
  started_ = false;
}

std::optional<std::string> RelayService::Failure() const {
  return std::nullopt;
}

} // namespace pbr::plus
