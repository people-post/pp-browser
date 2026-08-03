#include "base/media/CallAudioSession.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__ANDROID__)

#include <atomic>

#include <jni.h>
#include <SDL3/SDL.h>

namespace pbr {
namespace CallAudioSession {
namespace {

/** Match historical iOS DefaultToSpeaker: start loud unless the user turns it off. */
std::atomic<bool> g_speakerphone{true};
std::atomic<bool> g_session_active{false};

void ApplySpeakerphoneLocked(bool on) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return;
  }
  jmethodID mid = env->GetMethodID(cls, "setCallSpeakerphoneOn", "(Z)V");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  env->CallVoidMethod(activity, mid, on ? JNI_TRUE : JNI_FALSE);
  env->DeleteLocalRef(cls);
}

void ApplyVoipModeLocked(bool active) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return;
  }
  jmethodID mid = env->GetMethodID(cls, "setCallAudioSessionActive", "(Z)V");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  env->CallVoidMethod(activity, mid, active ? JNI_TRUE : JNI_FALSE);
  env->DeleteLocalRef(cls);
}

} // namespace

void ActivateForVoipCall() {
  g_session_active.store(true);
  ApplyVoipModeLocked(true);
  ApplySpeakerphoneLocked(g_speakerphone.load());
}

void Deactivate() {
  g_session_active.store(false);
  ApplySpeakerphoneLocked(false);
  ApplyVoipModeLocked(false);
}

bool SupportsSpeakerToggle() {
  return true;
}

bool IsSpeakerphoneOn() {
  return g_speakerphone.load();
}

void SetSpeakerphoneOn(bool on) {
  g_speakerphone.store(on);
  if (g_session_active.load()) {
    ApplySpeakerphoneLocked(on);
  }
}

} // namespace CallAudioSession
} // namespace pbr

#elif !defined(__APPLE__) || !TARGET_OS_IPHONE

namespace pbr {
namespace CallAudioSession {

void ActivateForVoipCall() {}
void Deactivate() {}

bool SupportsSpeakerToggle() {
  return false;
}

bool IsSpeakerphoneOn() {
  return false;
}

void SetSpeakerphoneOn(bool /*on*/) {}

} // namespace CallAudioSession
} // namespace pbr

#endif
