#pragma once

#include <functional>

namespace pbr {

class MeshControlPool;

/**
 * Process-wide mesh-control dispatch; installed by MeshHost while Amp is up.
 * Feature code posts Connect / IoPumpUntil waits here instead of AppRuntime::PostWorker.
 */
class MeshControlDispatch {
public:
  static void Install(MeshControlPool* pool);
  static void Uninstall();
  static bool IsInstalled();

  /** Post to MeshControlPool when installed; otherwise no-op (assert in debug). */
  static void Post(std::function<void()> task);
};

} // namespace pbr
