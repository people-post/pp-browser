#include "foundation/platform/DeviceScreenState.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

namespace pbr {

/** MainActivity.getCallScreenState(): bit 0 interactive, bit 1 keyguard locked; -1 when it could not ask. */
std::optional<DeviceScreenState> ReadDeviceScreenState() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) {
    return std::nullopt;
  }
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (activity == nullptr) {
    return std::nullopt;
  }
  std::optional<DeviceScreenState> out;
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetMethodID(cls, "getCallScreenState", "()I"); mid != nullptr) {
      const jint bits = env->CallIntMethod(activity, mid);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      } else if (bits >= 0) {
        out = DeviceScreenState{.interactive = (bits & 1) != 0, .keyguard_locked = (bits & 2) != 0};
      }
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return out;
}

} // namespace pbr
