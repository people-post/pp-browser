#include "feature/conversations/MeshNetworkReaction.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

logging::Logger& NetworkChangeLog() {
  static logging::Logger logger = logging::getLogger("NetworkChange");
  return logger;
}

} // namespace

mesh::LocalNetworkChange ToLocalNetworkChange(const NetworkChange& change) {
  mesh::LocalNetworkChange local;
  local.was_online = change.previous.online;
  local.online = change.current.online;
  local.attachment_changed = change.previous.fingerprint != change.current.fingerprint;
  return local;
}

std::optional<mesh::LocalNetworkChange> ReactMeshToNetworkChange(const NetworkChange& change, mesh::MeshHost* mesh) {
  if (change.generation == 0) {
    NetworkChangeLog().info << "network baseline online=" << (change.current.online ? 1 : 0)
                            << " transport=" << static_cast<int>(change.current.transport)
                            << " expensive=" << (change.current.expensive ? 1 : 0);
    return std::nullopt;
  }
  const mesh::LocalNetworkChange local = ToLocalNetworkChange(change);
  NetworkChangeLog().info << "network change gen=" << change.generation << " online=" << (local.was_online ? 1 : 0)
                          << "->" << (local.online ? 1 : 0) << " transport=" << static_cast<int>(change.previous.transport)
                          << "->" << static_cast<int>(change.current.transport)
                          << " expensive=" << (change.current.expensive ? 1 : 0)
                          << " attachment_changed=" << (local.attachment_changed ? 1 : 0);
  if (mesh) {
    mesh->OnLocalNetworkChanged(local);
  }
  return local;
}

} // namespace pbr
