#include "foundation/platform/StayConnectedService.h"

#include "foundation/runtime/AppLifecycle.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

namespace pbr {

namespace {

/** MainActivity.setStayConnected(boolean): starts / stops PpConnectionService. */
void SetConnectionService(const bool on) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls != nullptr) {
    if (jmethodID mid = env->GetMethodID(cls, "setStayConnected", "(Z)V"); mid != nullptr) {
      env->CallVoidMethod(activity, mid, static_cast<jboolean>(on ? JNI_TRUE : JNI_FALSE));
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
}

} // namespace

void InstallStayConnectedService() {
  static const AppLifecycle::ListenerId id = AppLifecycle::AddStayConnectedListener(SetConnectionService);
  (void)id;
  SetConnectionService(AppLifecycle::StaysConnected());
}

} // namespace pbr
