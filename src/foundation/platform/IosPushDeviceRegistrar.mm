#include "foundation/platform/IosPushDeviceRegistrar.h"

#include "foundation/platform/IPushDeviceRegistrar.h"
#if defined(PP_IOS_CALLKIT)
#include "foundation/platform/IosCallSystemUi.h"
#endif

#include <mutex>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif
#endif

namespace pbr {

namespace {
std::mutex g_token_mutex;
std::string g_cached_apns_token;
} // namespace

void SetIosCachedApnsToken(std::string token) {
  {
    std::lock_guard lock(g_token_mutex);
    g_cached_apns_token = std::move(token);
  }
  std::string copy;
  {
    std::lock_guard lock(g_token_mutex);
    copy = g_cached_apns_token;
  }
  if (!copy.empty()) {
    IPushDeviceRegistrar::NotifyTokenChanged(copy);
  }
}

bool IosPushDeviceRegistrar::IsSupported() const {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  return true;
#else
  return false;
#endif
}

void IosPushDeviceRegistrar::SetCachedToken(std::string token) {
  SetIosCachedApnsToken(std::move(token));
}

Roe<std::string> IosPushDeviceRegistrar::GetPushToken() {
  std::lock_guard lock(g_token_mutex);
  if (!g_cached_apns_token.empty()) {
    return g_cached_apns_token;
  }
  return Error("APNs token not available yet");
}

std::string IosPushDeviceRegistrar::CallPushToken() {
#if defined(PP_IOS_CALLKIT)
  return IosCachedVoipToken();
#else
  return {};
#endif
}

std::string IosPushDeviceRegistrar::DeviceId() const {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  @autoreleasepool {
    NSUUID* vendor = [UIDevice currentDevice].identifierForVendor;
    if (vendor != nil) {
      return std::string([[vendor UUIDString] UTF8String]);
    }
  }
#endif
  return "ios";
}

} // namespace pbr
