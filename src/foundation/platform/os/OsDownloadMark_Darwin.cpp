#include "foundation/platform/os/OsDownloadMark.h"

#include "common/PbrCompat.h"

#include <sys/xattr.h>

#include <cstdio>
#include <ctime>

namespace pbr::os {

// Same layout browsers write: "<flags>;<hex unix time>;<app name>;<uuid>" (uuid optional).
// 0081 = quarantined, user-approved-on-first-open prompt applies as for a browser download.
Roe<void> MarkFileAsDownloaded(const std::string& path) {
  char value[64];
  const int n = std::snprintf(value, sizeof(value), "0081;%llx;PP;", static_cast<unsigned long long>(std::time(nullptr)));
  if (n <= 0 || setxattr(path.c_str(), "com.apple.quarantine", value, static_cast<size_t>(n), 0, 0) != 0) {
    return Error("Failed to set quarantine attribute");
  }
  return {};
}

} // namespace pbr::os
