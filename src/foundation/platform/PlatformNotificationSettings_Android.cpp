// Android: reads what the phone's settings do to PP's alerts and opens the screen that fixes it. The questions are
// MainActivity.notificationHealth() / openNotificationFix(int); the banner style is an iOS thing (Unknown here).
#include "foundation/platform/PlatformNotificationSettings.h"

#include "common/PbrCompat.h"
#include "common/runtime/AppRuntime.h"

#include <jni.h>
#include <SDL3/SDL.h>

#include <utility>

namespace pbr {
namespace {

// MainActivity.notificationHealth() bits.
constexpr int kBlocked = 1;
constexpr int kSilenced = 2;
constexpr int kBatteryRestricted = 4;
constexpr int kNoPushInBuild = 8;
constexpr int kNoGoogleServices = 16;
constexpr int kAutostartVendor = 32;

/** Calls a static int / boolean method of MainActivity; false when the activity or method is missing or it threw. */
bool CallActivityStatic(const char* name, const char* signature, jint argument, bool takes_argument, bool is_boolean,
                        jint* result) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool ok = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = cls != nullptr ? env->GetStaticMethodID(cls, name, signature) : nullptr;
  if (mid != nullptr) {
    if (is_boolean) {
      *result = (takes_argument ? env->CallStaticBooleanMethod(cls, mid, argument)
                                : env->CallStaticBooleanMethod(cls, mid)) == JNI_TRUE
                    ? 1
                    : 0;
    } else {
      *result = takes_argument ? env->CallStaticIntMethod(cls, mid, argument) : env->CallStaticIntMethod(cls, mid);
    }
    ok = !env->ExceptionCheck();
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return ok;
}

NotificationHealth FromBits(int bits) {
  NotificationHealth health;
  if (bits < 0) {
    return health; // could not be read: nothing is claimed
  }
  health.notifications_blocked = (bits & kBlocked) != 0;
  health.silenced = (bits & kSilenced) != 0;
  health.battery_restricted = (bits & kBatteryRestricted) != 0;
  health.autostart_vendor = (bits & kAutostartVendor) != 0;
  if ((bits & kNoPushInBuild) != 0) {
    health.push_gap = PushGap::BuildHasNone;
  } else if ((bits & kNoGoogleServices) != 0) {
    health.push_gap = PushGap::NoGoogleServices;
  }
  return health;
}

} // namespace

void QueryBannerStyle(std::function<void(BannerStyle)> done) {
  if (!done) {
    return;
  }
  AppRuntime::PostUI([done = std::move(done)]() { done(BannerStyle::Unknown); });
}

bool OpenNotificationSettings() {
  return false;
}

void QueryNotificationHealth(std::function<void(NotificationHealth)> done) {
  if (!done) {
    return;
  }
  jint bits = -1;
  if (!CallActivityStatic("notificationHealth", "()I", 0, false, false, &bits)) {
    bits = -1;
  }
  AppRuntime::PostUI([done = std::move(done), bits]() { done(FromBits(bits)); });
}

bool OpenNotificationFix(NotificationFix fix) {
  jint target = 0;
  switch (fix) { // MainActivity.openNotificationFix(int)
    case NotificationFix::AppNotifications:
      target = 0;
      break;
    case NotificationFix::DoNotDisturb:
      target = 1;
      break;
    case NotificationFix::Battery:
      target = 2;
      break;
    case NotificationFix::Autostart:
      target = 3;
      break;
  }
  jint opened = 0;
  return CallActivityStatic("openNotificationFix", "(I)Z", target, true, true, &opened) && opened != 0;
}

} // namespace pbr
