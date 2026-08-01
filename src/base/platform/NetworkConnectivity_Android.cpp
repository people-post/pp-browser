#include "base/platform/NetworkConnectivity.h"

#if defined(__ANDROID__)

#include <jni.h>
#include <SDL3/SDL.h>

namespace pbr {

NetworkTransport QueryAndroidNetworkTransport() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return NetworkTransport::Unknown;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return NetworkTransport::Unknown;
  }
  jmethodID mid = env->GetMethodID(cls, "isActiveNetworkWifi", "()Z");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return NetworkTransport::Unknown;
  }
  const jboolean wifi = env->CallBooleanMethod(activity, mid);
  env->DeleteLocalRef(cls);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return NetworkTransport::Unknown;
  }
  return wifi ? NetworkTransport::Wifi : NetworkTransport::Cellular;
}

} // namespace pbr

#endif
