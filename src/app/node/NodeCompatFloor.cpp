#include "app/node/NodeCompatFloor.h"

#include "common/Logger.h"

#include <utility>

namespace pbr {

NodeCompatFloor::NodeCompatFloor(Fetch fetch, std::string node_version, std::function<int64_t()> steady_ms)
    : fetch_(std::move(fetch)), node_version_(std::move(node_version)), steady_ms_(std::move(steady_ms)) {}

std::string NodeCompatFloor::BelowMessage(const std::string& node_version, const std::string& floor) {
  return "this pp-node (" + node_version + ") is below the relay's min_node_version " + floor +
         ": it no longer registers or sends heartbeats there (the relay would answer 426); the mesh keeps running. "
         "Upgrade it.";
}

bool NodeCompatFloor::RelayCallsAllowed() {
  const int64_t now = steady_ms_ ? steady_ms_() : 0;
  if (!fetch_ || now < next_check_ms_) {
    return !below_;
  }
  next_check_ms_ = now + kRecheckMs;
  auto doc = fetch_();
  if (!doc) {
    logging::getLogger("pp-node").info << "client-compat not read (the floor read last stays): " << doc.error().message;
    return !below_;
  }
  const bool below = NodeBelowFloor(node_version_, *doc);
  floor_ = doc->min_node_version;
  if (below && !below_) {
    logging::getLogger("pp-node").error << BelowMessage(node_version_, floor_);
  } else if (!below && below_) {
    logging::getLogger("pp-node").info << "the relay's node floor no longer excludes this pp-node ("
                                       << (floor_.empty() ? std::string("none") : floor_)
                                       << "): registering there again";
  }
  below_ = below;
  return !below_;
}

} // namespace pbr
