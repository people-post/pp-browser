#include "feature/conversations/MeshNetworkReaction.h"

#include "common/Diag.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

mesh::LocalNetworkChange ToLocalNetworkChange(const NetworkChange& change) {
  mesh::LocalNetworkChange local;
  local.was_online = change.previous.online;
  local.online = change.current.online;
  local.attachment_changed = change.previous.fingerprint != change.current.fingerprint;
  return local;
}

std::optional<mesh::LocalNetworkChange> ReactMeshToNetworkChange(const NetworkChange& change, mesh::MeshHost* mesh) {
  if (change.generation == 0) {
    DiagLine("network.baseline")
        .Add("online", change.current.online)
        .Add("transport", static_cast<int>(change.current.transport))
        .Add("expensive", change.current.expensive)
        .Emit();
    return std::nullopt;
  }
  const mesh::LocalNetworkChange local = ToLocalNetworkChange(change);
  DiagLine("network.change")
      .Add("gen", change.generation)
      .Add("was_online", local.was_online)
      .Add("online", local.online)
      .Add("was_transport", static_cast<int>(change.previous.transport))
      .Add("transport", static_cast<int>(change.current.transport))
      .Add("expensive", change.current.expensive)
      .Add("attachment_changed", local.attachment_changed)
      .Emit();
  if (mesh) {
    mesh->OnLocalNetworkChanged(local);
  }
  return local;
}

} // namespace pbr
