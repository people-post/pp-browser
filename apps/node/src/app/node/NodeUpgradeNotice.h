#pragma once

#include <mutex>
#include <set>
#include <string>

namespace pbr {

/**
 * A relay answered this pp-node's heartbeat or registration `426 client_upgrade_required` (RelayPins'
 * SetOnUpgradeRequired): the operator reads one clear ERROR per relay per run, not a generic failure each period.
 * Thread-safe.
 */
class NodeUpgradeNotice {
public:
  /** Logs Message(`origin`) as an ERROR the first time `origin` is noted; true then, false after. */
  bool Note(const std::string& origin);
  /** "this pp-node is too old for the relay at <origin>: upgrade it". */
  static std::string Message(const std::string& origin);

private:
  std::mutex mutex_;
  std::set<std::string> noted_;
};

} // namespace pbr
