// Android: MainActivity.setUnreadBadgeCount(total). Only the vivo launcher takes a number that does not come from a
// notification; on every other maker the Java method does nothing (their icon numbers follow the notifications).
#include "foundation/platform/PlatformAppBadge.h"

#include <jni.h>
#include <SDL3/SDL.h>

namespace pbr {

void SetAppBadgeCount(int count) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()); // attaches a worker thread when it is not yet
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetMethodID(cls, "setUnreadBadgeCount", "(I)V"); mid != nullptr) {
      env->CallVoidMethod(activity, mid, static_cast<jint>(count > 0 ? count : 0));
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
