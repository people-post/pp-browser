#include "foundation/platform/os/OsProtectedData.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <time.h>

#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace pbr::os {

namespace {

std::mutex g_mutex;
std::map<ProtectedDataHandlerId, std::function<void()>> g_handlers;
ProtectedDataHandlerId g_next_id = 1;

/** MainActivity's ACTION_USER_PRESENT receiver (the Java main thread), through nativeOnUserPresent below. */
void RunHandlers() {
  std::vector<std::function<void()>> handlers;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    handlers.reserve(g_handlers.size());
    for (const auto& entry : g_handlers) {
      handlers.push_back(entry.second);
    }
  }
  for (const auto& fn : handlers) {
    if (fn) {
      fn();
    }
  }
}

/**
 * Calls a static method of the running activity's class (MainActivity) returning a boolean or an int (`as_int`).
 * False when there is no activity or method, or it threw. FindClass from a native thread does not see app classes.
 */
bool CallActivity(const char* name, const char* signature, const bool as_int, jint* result) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool ok = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = cls != nullptr ? env->GetStaticMethodID(cls, name, signature) : nullptr;
  if (mid != nullptr) {
    *result = as_int ? env->CallStaticIntMethod(cls, mid) : (env->CallStaticBooleanMethod(cls, mid) == JNI_TRUE);
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

} // namespace

bool ProtectedDataAvailable() {
  // KeyguardManager.isDeviceLocked: the lock screen is up. When it cannot be asked, "available", and the secure-blob
  // read that follows is what decides (the Keystore refuses the grace key while the device is locked).
  jint locked = 0;
  if (!CallActivity("isDeviceLocked", "()Z", false, &locked)) {
    return true;
  }
  return locked == 0;
}

ProtectedDataHandlerId OnProtectedDataAvailable(std::function<void()> fn) {
  // MainActivity's receiver lives as long as the activity; the registry decides who hears about it.
  std::lock_guard<std::mutex> lock(g_mutex);
  const ProtectedDataHandlerId id = g_next_id++;
  g_handlers.emplace(id, std::move(fn));
  return id;
}

void RemoveProtectedDataHandler(const ProtectedDataHandlerId id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handlers.erase(id);
}

GraceClock GraceClockNow() {
  GraceClock now;
  // Settings.Global.BOOT_COUNT: one more each boot. Byte 0 = 1 (the format), bytes 1-8 the count big-endian.
  // Without it there is no clock at all (mono_seconds stays 0 and GraceUnlock fails closed): CLOCK_BOOTTIME
  // restarts at boot, so an all-zero id would let a window outlive a reboot.
  // A count of 0 was never incremented (a ROM that does not keep it): treated as unreadable.
  jint count = -1;
  if (!CallActivity("bootCount", "()I", true, &count) || count <= 0) {
    return now;
  }
  const uint64_t value = static_cast<uint64_t>(count);
  now.boot_id[0] = 1;
  for (int i = 0; i < 8; ++i) {
    now.boot_id[1 + i] = static_cast<uint8_t>(value >> (8 * (7 - i)));
  }
  // CLOCK_BOOTTIME (SystemClock.elapsedRealtime) keeps counting while the device sleeps (CLOCK_MONOTONIC does not).
  timespec ts{};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
    now.mono_seconds = static_cast<int64_t>(ts.tv_sec);
  } else {
    // No clock: 0 tells GraceUnlock there is none, and it fails closed (no window opened or used).
    now.mono_seconds = 0;
  }
  return now;
}

} // namespace pbr::os

/** MainActivity's ACTION_USER_PRESENT receiver (the Java main thread): the lock screen went away. */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_MainActivity_nativeOnUserPresent(JNIEnv*, jclass) {
  pbr::os::RunHandlers();
}
