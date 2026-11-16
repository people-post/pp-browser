#include "app/node/NodeUpgradeNotice.h"

#include "common/Logger.h"

#include "common/PbrCompat.h"

namespace pbr {

bool NodeUpgradeNotice::Note(const std::string& origin) {
  {
    std::lock_guard lock(mutex_);
    if (!noted_.insert(origin).second) {
      return false;
    }
  }
  logging::getLogger("pp-node").error << Message(origin);
  return true;
}

std::string NodeUpgradeNotice::Message(const std::string& origin) {
  return "this pp-node is too old for the relay at " + origin +
         ": upgrade it (the relay answers its registration and heartbeats 426 client_upgrade_required)";
}

} // namespace pbr
