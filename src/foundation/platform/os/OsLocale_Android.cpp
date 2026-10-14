#include "foundation/platform/os/OsLocale.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <string>
#include <vector>

namespace pbr::os {

// Android apps carry no LANG / LC_* variables, so the POSIX reader found nothing and the UI stayed
// English (B80). Ask MainActivity for LocaleList.getDefault().toLanguageTags() instead.
std::vector<std::string> PreferredSystemLocales() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return {};
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return {};
  }
  jmethodID mid = env->GetMethodID(cls, "getPreferredLanguageTags", "()Ljava/lang/String;");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return {};
  }
  jstring jtags = static_cast<jstring>(env->CallObjectMethod(activity, mid));
  if (jtags == nullptr) {
    env->DeleteLocalRef(cls);
    return {};
  }
  std::string csv;
  if (const char* utf = env->GetStringUTFChars(jtags, nullptr)) {
    csv = utf;
    env->ReleaseStringUTFChars(jtags, utf);
  }
  env->DeleteLocalRef(jtags);
  env->DeleteLocalRef(cls);
  return SplitLanguageTags(csv);
}

} // namespace pbr::os
