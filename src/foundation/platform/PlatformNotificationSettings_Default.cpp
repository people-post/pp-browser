// Platforms without a notification banner style to read (everything but iOS): Unknown, nothing to open.
#include "foundation/platform/PlatformNotificationSettings.h"

#include "common/PbrCompat.h"
#include "common/runtime/AppRuntime.h"

#include <utility>

namespace pbr {

void QueryBannerStyle(std::function<void(BannerStyle)> done) {
  if (!done) {
    return;
  }
  AppRuntime::PostUI([done = std::move(done)]() { done(BannerStyle::Unknown); });
}

bool OpenNotificationSettings() {
  return false;
}

} // namespace pbr
