#include "foundation/platform/os/OsDownloadMark.h"

#include "common/PbrCompat.h"

namespace pbr::os {

// No provenance mark on this platform.
Roe<void> MarkFileAsDownloaded(const std::string& /*path*/) {
  return {};
}

} // namespace pbr::os
