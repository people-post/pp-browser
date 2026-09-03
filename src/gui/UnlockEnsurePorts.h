#pragma once

#include <functional>

namespace pbr {

/**
 * Profile unlock ensure ports for chat / settings / contacts / people-picker.
 * Declared in UI (consumers); Application fills from owned ProfileUnlockGate.
 * Clear via BindUnlockEnsure({}).
 */
struct UnlockEnsurePorts {
  std::function<void(std::function<void(bool unlocked)>)> ensure_unlocked;
  std::function<bool()> is_unlock_in_progress;
};

} // namespace pbr
