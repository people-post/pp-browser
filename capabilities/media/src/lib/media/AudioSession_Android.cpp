#if defined(__ANDROID__)

#include "lib/media/AudioSession.h"

#include <atomic>
#include <optional>

#include <jni.h>
#include <SDL3/SDL.h>

namespace pbr {
namespace AudioSession {
namespace {

/** The call's speaker choice: default earpiece; kept across Deactivate (a media rebuild), cleared when the call ends. */
std::atomic<bool> g_speakerphone{false};
std::atomic<bool> g_session_active{false};

/**
 * MainActivity, for one call into it. The activity's local reference and its class's go when it does: the media
 * threads SDL attached to the VM never detach, so local references made there are never freed otherwise. An
 * exception the call threw is cleared (a pending one would abort the next JNI call). `env`: the caller's (a native
 * method's), else SDL's for this thread.
 */
class ActivityCall {
public:
  explicit ActivityCall(JNIEnv* env = nullptr)
      : env_(env != nullptr ? env : static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv())) {
    if (env_ == nullptr) {
      return;
    }
    activity_ = static_cast<jobject>(SDL_GetAndroidActivity());
    if (activity_ != nullptr) {
      cls_ = env_->GetObjectClass(activity_);
    }
  }
  ~ActivityCall() {
    if (cls_ != nullptr) {
      env_->DeleteLocalRef(cls_);
    }
    if (activity_ != nullptr) {
      env_->DeleteLocalRef(activity_);
    }
  }
  ActivityCall(const ActivityCall&) = delete;
  ActivityCall& operator=(const ActivityCall&) = delete;

  void Void(const char* method, const bool arg) {
    if (jmethodID mid = Method(method, "(Z)V"); mid != nullptr) {
      env_->CallVoidMethod(activity_, mid, arg ? JNI_TRUE : JNI_FALSE);
      ClearedException();
    }
  }

  /** Nothing: Java could not be asked (no activity yet, no such method, it threw). */
  std::optional<bool> Bool(const char* method) {
    jmethodID mid = Method(method, "()Z");
    if (mid == nullptr) {
      return std::nullopt;
    }
    const jboolean out = env_->CallBooleanMethod(activity_, mid);
    if (ClearedException()) {
      return std::nullopt;
    }
    return out == JNI_TRUE;
  }

private:
  jmethodID Method(const char* name, const char* signature) {
    if (cls_ == nullptr) {
      return nullptr;
    }
    jmethodID mid = env_->GetMethodID(cls_, name, signature);
    if (mid == nullptr) {
      env_->ExceptionClear();
    }
    return mid;
  }

  bool ClearedException() {
    if (!env_->ExceptionCheck()) {
      return false;
    }
    env_->ExceptionClear();
    return true;
  }

  JNIEnv* env_ = nullptr;
  jobject activity_ = nullptr;
  jclass cls_ = nullptr;
};

void ApplySpeakerphoneLocked(bool on) {
  ActivityCall().Void("setVoipSpeakerphoneOn", on);
}

void ApplyVoipModeLocked(bool active) {
  ActivityCall().Void("setAudioSessionActive", active);
}

/** Cached: distinct earpiece vs loudspeaker (false on most tablets). */
std::atomic<int> g_has_earpiece{-1}; // -1 unknown, 0 no, 1 yes

bool QueryHasEarpieceRoute() {
  const int cached = g_has_earpiece.load(std::memory_order_acquire);
  if (cached >= 0) {
    return cached != 0;
  }
  const std::optional<bool> has = ActivityCall().Bool("hasEarpieceRoute");
  if (!has) {
    return true; // fail open: keep toggle until Activity is ready
  }
  g_has_earpiece.store(*has ? 1 : 0, std::memory_order_release);
  return *has;
}

} // namespace

void ActivateForVoip() {
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
  ClearInterruption();
  ApplySpeakerphoneLocked(false);
  ApplyVoipModeLocked(false);
}

void CancelPendingDeactivate() {}

// Voice messages: SDL opens playback with the media role (speaker, media volume) and capture needs no switch.
void ActivateForVoicePlayback() {}
void ActivateForVoiceRecording() {}
void ReleaseVoice() {}

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

bool SpeakerToggleNeedsDeviceReopen() {
  return true;
}

int OpusEncoderComplexity() {
  return 5;
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
  // Stock SDL 3.4.16 (B35): "Communications" role → AAUDIO_USAGE_VOICE_COMMUNICATION; input
  // preset 7 = VOICE_COMMUNICATION engages the OEM AEC/NS. Speaker-only tablets keep MEDIA
  // usage (paired with MODE_NORMAL in MainActivity; IN_COMMUNICATION+MEDIA is whisper-quiet).
  if (SupportsSpeakerToggle()) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "Communications");
    SDL_SetHint(SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET, "7");
  } else {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "");
    SDL_SetHint(SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET, "");
  }
}

void ClearCaptureAudioHints() {
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "");
  SDL_SetHint(SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET, "");
}

} // namespace AudioSession
} // namespace pbr

/**
 * MainActivity's call audio-focus listener (main thread): `lost` on a loss (a phone call, another app's call), not
 * on a regain. A loss outside a live session, or a regain without a loss, changes nothing.
 */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_MainActivity_nativeOnCallAudioFocus(JNIEnv*, jclass,
                                                                                               jboolean lost) {
  if (lost == JNI_TRUE) {
    if (pbr::AudioSession::g_session_active.load()) {
      SDL_Log("AudioSession: call audio focus lost — interrupted");
      pbr::AudioSession::NotifyInterruptionBegan();
    }
  } else if (pbr::AudioSession::IsInterrupted()) {
    SDL_Log("AudioSession: call audio focus back — resuming");
    pbr::AudioSession::NotifyInterruptionEnded();
  }
}

#endif // defined(__ANDROID__)
