#include "lib/media/DeviceVitals.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <sys/resource.h>

namespace pbr {

namespace {

/** MainActivity.getDeviceVitals(): {power saver, thermal 0..3 or -1, battery percent or -1, charging}. */
void ReadActivityVitals(DeviceVitals& v) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return;
  }
  jmethodID mid = env->GetMethodID(cls, "getDeviceVitals", "()[I");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  auto values = static_cast<jintArray>(env->CallObjectMethod(activity, mid));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  if (values != nullptr && env->GetArrayLength(values) >= 4) {
    jint out[4] = {0, -1, -1, 0};
    env->GetIntArrayRegion(values, 0, 4, out);
    v.low_power = out[0] != 0;
    v.thermal = out[1];
    v.battery_pct = out[2];
    v.charging = out[3] != 0;
  }
  if (values != nullptr) {
    env->DeleteLocalRef(values);
  }
  env->DeleteLocalRef(cls);
}

} // namespace

DeviceVitals ReadDeviceVitals() {
  DeviceVitals v;
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    v.cpu_s = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
              static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
  }
  ReadActivityVitals(v);
  return v;
}

} // namespace pbr
