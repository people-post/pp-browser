#if defined(__ANDROID__)

#include "domain/media/CallAudioSession.h"

#include <atomic>

#include <jni.h>
#include <SDL3/SDL.h>

namespace pbr {
namespace CallAudioSession {
namespace {

/** Default earpiece; user opts into speakerphone. Reset each call on Deactivate. */
std::atomic<bool> g_speakerphone{false};
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

/** Cached: distinct earpiece vs loudspeaker (false on most tablets). */
std::atomic<int> g_has_earpiece{-1}; // -1 unknown, 0 no, 1 yes

bool QueryHasEarpieceRoute() {
  const int cached = g_has_earpiece.load(std::memory_order_acquire);
  if (cached >= 0) {
    return cached != 0;
  }
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return true; // fail open: keep toggle until Activity is ready
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return true;
  }
  jmethodID mid = env->GetMethodID(cls, "hasCallEarpieceRoute", "()Z");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return true;
  }
  const bool has = env->CallBooleanMethod(activity, mid) == JNI_TRUE;
  env->DeleteLocalRef(cls);
  g_has_earpiece.store(has ? 1 : 0, std::memory_order_release);
  return has;
}

} // namespace

void ActivateForVoipCall() {
  const bool was_active = g_session_active.exchange(true);
  ApplyVoipModeLocked(true);
  // Re-applying speaker on every SDL reopen races OEM route changes (Moto) and can
  // kill AAudio capture mid-open. Only push the route when entering a call session.
  if (!was_active) {
    // Tablets with no earpiece: only loudspeaker exists — start on speaker so we don't
    // sit on FORCE_NONE with the same device and a quieter comms gain curve.
    if (!QueryHasEarpieceRoute()) {
      g_speakerphone.store(true);
    }
    ApplySpeakerphoneLocked(g_speakerphone.load());
  }
}

void Deactivate() {
  g_session_active.store(false);
  g_speakerphone.store(false);
  ApplySpeakerphoneLocked(false);
  ApplyVoipModeLocked(false);
}

bool SupportsSpeakerToggle() {
  return QueryHasEarpieceRoute();
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

int CaptureOpenAttemptCount() {
  return 4;
}

int CaptureOpenRetryDelayMs(int attempt_index) {
  constexpr int kRetryDelayMs[] = {0, 80, 200, 400};
  if (attempt_index < 0 || attempt_index >= static_cast<int>(sizeof(kRetryDelayMs) / sizeof(kRetryDelayMs[0]))) {
    return 0;
  }
  return kRetryDelayMs[attempt_index];
}

int CaptureReopenSettleDelayMs() {
  return 150;
}

void ApplyCaptureAudioHints() {
  // Phones: VoIP usage so MODE_IN_COMMUNICATION doesn't duck MEDIA (see SDL_aaudio patch).
  // Speaker-only tablets: leave default MEDIA — paired with MODE_NORMAL in MainActivity
  // (API < 28 cannot set AAUDIO_USAGE_VOICE_COMMUNICATION; IN_COMMUNICATION+MEDIA is whisper-quiet).
  if (SupportsSpeakerToggle()) {
    SDL_SetHint("SDL_ANDROID_AAUDIO_VOICE_COMMUNICATION", "1");
  } else {
    SDL_SetHint("SDL_ANDROID_AAUDIO_VOICE_COMMUNICATION", "0");
  }
}

void ClearCaptureAudioHints() {
  SDL_SetHint("SDL_ANDROID_AAUDIO_VOICE_COMMUNICATION", "0");
}

} // namespace CallAudioSession
} // namespace pbr

#endif // defined(__ANDROID__)
