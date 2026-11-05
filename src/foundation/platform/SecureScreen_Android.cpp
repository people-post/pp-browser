#include "foundation/platform/SecureScreen.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

namespace pbr::SecureScreen {

/** MainActivity.setSecureScreen(boolean): FLAG_SECURE on the activity's window. */
void SetSecretOnScreen(const bool on) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetMethodID(cls, "setSecureScreen", "(Z)V"); mid != nullptr) {
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

} // namespace pbr::SecureScreen
