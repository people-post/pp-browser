#include "feature/calls/CallHopService.h"

#include "common/PbrCompat.h"

namespace pbr {

CallHopService::CallHopService(ICallSessionStore& sessions, const IContactBook& contacts)
    : ranking_(contacts), planning_(sessions, ranking_) {
  ranking_.SetMediaRelayDeps(&relay_deps_);
  planning_.SetMediaRelayDeps(&relay_deps_);
}

} // namespace pbr
