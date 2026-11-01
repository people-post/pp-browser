#include "lib/messaging/AttachmentPlatform.h"

#include <mutex>
#include <utility>

namespace pbr {

namespace {

std::mutex& PlatformMutex() {
  static std::mutex mutex;
  return mutex;
}

AttachmentPlatform& Installed() {
  static AttachmentPlatform platform;
  return platform;
}

} // namespace

void SetAttachmentPlatform(AttachmentPlatform platform) {
  std::lock_guard lock(PlatformMutex());
  Installed() = std::move(platform);
}

AttachmentPlatform CurrentAttachmentPlatform() {
  std::lock_guard lock(PlatformMutex());
  return Installed();
}

} // namespace pbr
