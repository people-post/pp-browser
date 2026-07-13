#include "base/platform/AndroidPushDeviceRegistrar.h"

#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL.h>
#endif

namespace pbr {

namespace {
std::mutex g_token_mutex;
std::string g_cached_fcm_token;
} // namespace

void SetAndroidCachedFcmToken(std::string token) {
  std::lock_guard lock(g_token_mutex);
  g_cached_fcm_token = std::move(token);
}

bool AndroidPushDeviceRegistrar::IsSupported() const {
#if defined(__ANDROID__)
  return true;
#else
  return false;
#endif
}

void AndroidPushDeviceRegistrar::SetCachedToken(std::string token) {
  SetAndroidCachedFcmToken(std::move(token));
}

Roe<std::string> AndroidPushDeviceRegistrar::GetPushToken() {
  std::lock_guard lock(g_token_mutex);
  if (!g_cached_fcm_token.empty()) {
    return g_cached_fcm_token;
  }
  return Error("FCM token not available yet");
}

std::string AndroidPushDeviceRegistrar::DeviceId() const {
#if defined(__ANDROID__)
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return "android";
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return "android";
  }
  jmethodID mid = env->GetMethodID(cls, "getStableDeviceId", "()Ljava/lang/String;");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return "android";
  }
  jstring jid = static_cast<jstring>(env->CallObjectMethod(activity, mid));
  if (jid == nullptr) {
    env->DeleteLocalRef(cls);
    return "android";
  }
  const char* utf = env->GetStringUTFChars(jid, nullptr);
  std::string out = utf ? utf : "android";
  if (utf) {
    env->ReleaseStringUTFChars(jid, utf);
  }
  env->DeleteLocalRef(jid);
  env->DeleteLocalRef(cls);
  return out;
#else
  return "android";
#endif
}

} // namespace pbr

#if defined(__ANDROID__)
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpPushBridge_nativeSetFcmToken(JNIEnv* env, jclass,
                                                                                         jstring token) {
  if (env == nullptr || token == nullptr) {
    return;
  }
  const char* utf = env->GetStringUTFChars(token, nullptr);
  if (utf == nullptr) {
    return;
  }
  pbr::SetAndroidCachedFcmToken(utf);
  env->ReleaseStringUTFChars(token, utf);
}
#endif
