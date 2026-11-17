#include "foundation/platform/os/OsAppState.h"

#include <SDL3/SDL.h>
#include <jni.h>

namespace pbr::os {

bool AppShownNow() {
  // MainActivity.isAppShown: between onStart and onStop, so false while the app runs behind another one or the lock
  // screen (a push or a sync there must not extend the PIN grace window). No activity: not shown.
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool shown = false; // not shown when the activity cannot be asked
  // The activity's class: FindClass from a native thread does not see app classes.
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = cls != nullptr ? env->GetStaticMethodID(cls, "isAppShown", "()Z") : nullptr;
  if (mid != nullptr) {
    const jboolean answer = env->CallStaticBooleanMethod(cls, mid);
    if (!env->ExceptionCheck()) {
      shown = answer == JNI_TRUE;
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return shown;
}

} // namespace pbr::os
