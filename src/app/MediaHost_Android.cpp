#include "app/MediaHost.h"

#include "lib/media/AudioSession.h"
#include "lib/media/MediaHost.h"

#include <optional>
#include <utility>

#include <jni.h>
#include <SDL3/SDL.h>

namespace pbr {

namespace {

/**
 * The JNIEnv of the activity's native callback running on this thread (its main thread), if any: the ports media calls
 * from inside one use it, as the callback's own; every other thread takes SDL's.
 */
thread_local JNIEnv* t_callback_env = nullptr;

class CallbackEnvScope {
public:
  explicit CallbackEnvScope(JNIEnv* env) : previous_(t_callback_env) { t_callback_env = env; }
  ~CallbackEnvScope() { t_callback_env = previous_; }
  CallbackEnvScope(const CallbackEnvScope&) = delete;
  CallbackEnvScope& operator=(const CallbackEnvScope&) = delete;

private:
  JNIEnv* previous_;
};

/**
 * MainActivity, for one call into it. The activity's local reference and its class's go when it does: the media
 * threads SDL attached to the VM never detach, so local references made there are never freed otherwise. An
 * exception the call threw is cleared (a pending one would abort the next JNI call).
 */
class ActivityCall {
public:
  ActivityCall() : env_(t_callback_env != nullptr ? t_callback_env : static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv())) {
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

  /** False when Java could not be asked (no activity yet, no such method) or it threw. */
  bool Void(const char* method, const bool arg) {
    jmethodID mid = Method(method, "(Z)V");
    if (mid == nullptr) {
      return false;
    }
    env_->CallVoidMethod(activity_, mid, arg ? JNI_TRUE : JNI_FALSE);
    return !ClearedException();
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

  /**
   * MainActivity.getDeviceVitals(): {power saver, thermal 0..3 or -1, battery percent or -1, charging}. Nothing: Java
   * could not be asked, or gave fewer than four.
   */
  std::optional<DeviceVitals> Vitals() {
    jmethodID mid = Method("getDeviceVitals", "()[I");
    if (mid == nullptr) {
      return std::nullopt;
    }
    auto values = static_cast<jintArray>(env_->CallObjectMethod(activity_, mid));
    ClearedException();
    std::optional<DeviceVitals> out;
    if (values != nullptr && env_->GetArrayLength(values) >= 4) {
      jint read[4] = {0, -1, -1, 0};
      env_->GetIntArrayRegion(values, 0, 4, read);
      out.emplace();
      out->low_power = read[0] != 0;
      out->thermal = read[1];
      out->battery_pct = read[2];
      out->charging = read[3] != 0;
    }
    if (values != nullptr) {
      env_->DeleteLocalRef(values);
    }
    return out;
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

} // namespace

void InstallMediaHost() {
  MediaHost host;
  host.set_call_audio_mode = [](bool active) { return ActivityCall().Void("setAudioSessionActive", active); };
  host.set_speakerphone = [](bool on) { return ActivityCall().Void("setVoipSpeakerphoneOn", on); };
  host.has_earpiece_route = []() { return ActivityCall().Bool("hasEarpieceRoute"); };
  host.retake_call_audio_focus = []() { return ActivityCall().Bool("retakeCallAudioFocus"); };
  host.read_device_vitals = []() { return ActivityCall().Vitals(); };
  SetMediaHost(std::move(host));
}

} // namespace pbr

/**
 * MainActivity's call audio-focus listener (main thread): AudioManager's AUDIOFOCUS_* change, for media's session
 * (AudioFocusInterruption reads it).
 */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_MainActivity_nativeOnCallAudioFocus(JNIEnv* env, jclass,
                                                                                               jint focus_change) {
  pbr::CallbackEnvScope scope(env);
  pbr::AudioSession::NotifyHostCallAudioFocus(focus_change);
}

/** MainActivity.onResume (main thread): the user is back; a call's focus lost for good is asked for again. */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_MainActivity_nativeRetakeCallAudioFocus(JNIEnv* env,
                                                                                                   jclass) {
  pbr::CallbackEnvScope scope(env);
  pbr::AudioSession::NotifyHostResumed();
}
