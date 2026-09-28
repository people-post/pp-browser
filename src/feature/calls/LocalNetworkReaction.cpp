#include "feature/calls/LocalNetworkReaction.h"

#include "common/Logger.h"
#include "domain/mesh/host/MeshHost.h"
#include "feature/calls/CallStack.h"

namespace pbr {
namespace {

logging::Logger& NetworkChangeLog() {
  static logging::Logger logger = logging::getLogger("NetworkChange");
  return logger;
}

} // namespace

LocalNetworkChange ToLocalNetworkChange(const NetworkChange& change) {
  LocalNetworkChange local;
  local.was_online = change.previous.online;
  local.online = change.current.online;
  local.attachment_changed = change.previous.fingerprint != change.current.fingerprint;
  return local;
}

MobilityAttachment ToMobilityAttachment(const NetworkState& state) {
  MobilityAttachment attachment;
  attachment.online = state.online;
  attachment.cellular = state.transport == NetworkTransport::Cellular;
  attachment.expensive = state.expensive;
  return attachment;
}

void ReactToNetworkChange(const NetworkChange& change, MeshHost* mesh, CallStack* calls) {
  if (change.generation == 0) {
    NetworkChangeLog().info << "network baseline online=" << (change.current.online ? 1 : 0)
                            << " transport=" << static_cast<int>(change.current.transport)
                            << " expensive=" << (change.current.expensive ? 1 : 0);
    if (calls) {
      calls->OnLocalNetwork(ToMobilityAttachment(change.current), /*changed=*/false, /*moved=*/false);
    }
    return;
  }
  const LocalNetworkChange local = ToLocalNetworkChange(change);
  NetworkChangeLog().info << "network change gen=" << change.generation << " online=" << (local.was_online ? 1 : 0)
                          << "->" << (local.online ? 1 : 0) << " transport=" << static_cast<int>(change.previous.transport)
                          << "->" << static_cast<int>(change.current.transport)
                          << " expensive=" << (change.current.expensive ? 1 : 0)
                          << " attachment_changed=" << (local.attachment_changed ? 1 : 0);
  if (mesh) {
    mesh->OnLocalNetworkChanged(local);
  }
  if (calls) {
    calls->OnLocalNetwork(ToMobilityAttachment(change.current), /*changed=*/local.attachment_changed,
                          /*moved=*/DecideLocalNetworkReaction(local).probe_links);
  }
}

} // namespace pbr
