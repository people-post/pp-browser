#include "foundation/platform/AndroidSystemChrome.h"

#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL.h>
#endif

namespace pbr {

void AndroidSystemChrome::SetAppearance(const std::string& appearance) {
#if defined(__ANDROID__)
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return;
  }
  jmethodID mid = env->GetMethodID(cls, "setAppAppearance", "(Ljava/lang/String;)V");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  const char* value = appearance.c_str();
  if (appearance != "light" && appearance != "dark") {
    value = "system";
  }
  jstring j_appearance = env->NewStringUTF(value);
  env->CallVoidMethod(activity, mid, j_appearance);
  env->DeleteLocalRef(j_appearance);
  env->DeleteLocalRef(cls);
#else
  (void)appearance;
#endif
}

} // namespace pbr
