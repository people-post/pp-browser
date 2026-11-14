#include "foundation/platform/os/OsDeviceName.h"

#include "common/DeviceName.h"

#include <SDL3/SDL.h>
#include <jni.h>

namespace pbr::os {

// MainActivity.deviceName(): Settings.Global.DEVICE_NAME, else the maker and model. Main thread only (startup).
std::string SystemDeviceName() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return {};
  }
  std::string raw;
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetStaticMethodID(cls, "deviceName", "()Ljava/lang/String;"); mid != nullptr) {
      auto jname = static_cast<jstring>(env->CallStaticObjectMethod(cls, mid));
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      } else if (jname != nullptr) {
        if (const char* utf = env->GetStringUTFChars(jname, nullptr)) {
          raw = utf;
          env->ReleaseStringUTFChars(jname, utf);
        }
        env->DeleteLocalRef(jname);
      }
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return NormalizeDeviceName(raw);
}

} // namespace pbr::os
