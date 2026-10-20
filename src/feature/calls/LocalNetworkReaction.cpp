#include "feature/calls/LocalNetworkReaction.h"

#include "feature/calls/CallStack.h"
#include "common/MeshAlias.h"

namespace pbr {

MobilityAttachment ToMobilityAttachment(const NetworkState& state) {
  MobilityAttachment attachment;
  attachment.online = state.online;
  attachment.cellular = state.transport == NetworkTransport::Cellular;
  attachment.expensive = state.expensive;
  return attachment;
}

void ReactCallsToNetworkChange(const NetworkChange& change, const mesh::LocalNetworkChange* local, CallStack& calls) {
  if (!local) {
    calls.OnLocalNetwork(ToMobilityAttachment(change.current), /*changed=*/false, /*moved=*/false);
    return;
  }
  calls.OnLocalNetwork(ToMobilityAttachment(change.current), /*changed=*/local->attachment_changed,
                       /*moved=*/mesh::DecideLocalNetworkReaction(*local).probe_links);
}

} // namespace pbr
