#include "foundation/platform/DeviceRinger.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

namespace pbr {

/** MainActivity.getRingerState(): {ringer mode, interruption filter, ring volume}; null when it could not ask. */
std::optional<DeviceRingerReading> ReadDeviceRinger() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) {
    return std::nullopt;
  }
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (activity == nullptr) {
    return std::nullopt;
  }
  std::optional<DeviceRingerReading> out;
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetMethodID(cls, "getRingerState", "()[I"); mid != nullptr) {
      auto values = static_cast<jintArray>(env->CallObjectMethod(activity, mid));
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      } else if (values != nullptr && env->GetArrayLength(values) >= 3) {
        jint raw[3] = {0, 0, 0};
        env->GetIntArrayRegion(values, 0, 3, raw);
        out = DeviceRingerReading{.ringer_mode = raw[0], .interruption_filter = raw[1], .ring_volume = raw[2]};
      }
      if (values != nullptr) {
        env->DeleteLocalRef(values);
      }
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return out;
}

/** MainActivity.setRingVibration(boolean). */
void SetDeviceRingVibration(const bool on) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) {
    return;
  }
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (activity == nullptr) {
    return;
  }
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetMethodID(cls, "setRingVibration", "(Z)V"); mid != nullptr) {
      env->CallVoidMethod(activity, mid, static_cast<jboolean>(on ? JNI_TRUE : JNI_FALSE));
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
}

} // namespace pbr
