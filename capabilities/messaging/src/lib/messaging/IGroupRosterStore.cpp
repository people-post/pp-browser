#include "lib/messaging/IGroupRosterStore.h"

namespace pbr {

std::unique_lock<std::recursive_mutex> IGroupRosterStore::LockForUpdate() {
  static std::recursive_mutex mutex;
  return std::unique_lock<std::recursive_mutex>(mutex);
}

} // namespace pbr
