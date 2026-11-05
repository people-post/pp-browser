#include "foundation/platform/OngoingCallService.h"

#include "common/runtime/AppRuntime.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

#include <mutex>
#include <utility>

namespace pbr::OngoingCallService {

namespace {

std::mutex g_mu;
std::function<void()> g_hang_up;

} // namespace

/** MainActivity.setOngoingCall(boolean, String): starts, updates or stops PpCallService. */
void SetOngoingCall(const bool on, const std::string& title) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetMethodID(cls, "setOngoingCall", "(ZLjava/lang/String;)V"); mid != nullptr) {
      jstring j_title = env->NewStringUTF(title.c_str());
      env->CallVoidMethod(activity, mid, static_cast<jboolean>(on ? JNI_TRUE : JNI_FALSE), j_title);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      env->DeleteLocalRef(j_title);
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
}

void SetHangUpHandler(std::function<void()> fn) {
  std::lock_guard lock(g_mu);
  g_hang_up = std::move(fn);
}

} // namespace pbr::OngoingCallService

/** Hang up on the ongoing-call notification (PpCallActionReceiver, the main thread). */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpCallService_nativeOnHangUp(JNIEnv*, jclass) {
  pbr::AppRuntime::PostUI([]() {
    std::function<void()> fn;
    {
      std::lock_guard lock(pbr::OngoingCallService::g_mu);
      fn = pbr::OngoingCallService::g_hang_up;
    }
    if (fn) {
      fn();
    }
  });
}
