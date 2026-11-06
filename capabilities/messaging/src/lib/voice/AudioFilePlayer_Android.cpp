// Android: a sound file plays through MediaPlayer (PpAudioFilePlayer.java, media usage: the loudspeaker at media
// volume). Java reports progress and the end through two native methods, tagged with the Play's token so a file that
// was replaced or stopped reports nothing.
#include "lib/voice/AudioFilePlayer.h"

#include <jni.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <mutex>

namespace pbr {
namespace {

std::mutex g_mutex;
int64_t g_token = 0; // the Play whose reports count; 0: none
/** Tokens for the whole app (g_token is too): two players never hand out the same one, so neither takes the other's
 *  late reports for its own. */
std::atomic<int64_t> g_next_token{0};
AudioFilePlayerCallbacks g_callbacks;

/** Calls a static method of the running activity's class (MainActivity) with a long argument (or none). */
bool CallActivity(const char* name, const char* signature, const jvalue* args, bool* result) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool ok = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = cls != nullptr ? env->GetStaticMethodID(cls, name, signature) : nullptr;
  if (mid != nullptr) {
    if (result != nullptr) {
      *result = env->CallStaticBooleanMethodA(cls, mid, args) == JNI_TRUE;
    } else {
      env->CallStaticVoidMethodA(cls, mid, args);
    }
    ok = !env->ExceptionCheck();
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return ok;
}

class AndroidAudioFilePlayer final : public IAudioFilePlayer {
public:
  ~AndroidAudioFilePlayer() override { Stop(); }

  bool Play(const std::string& path, AudioFilePlayerCallbacks callbacks) override {
    int64_t token = 0;
    {
      std::lock_guard lock(g_mutex);
      token = ++g_next_token;
      g_token = token;
      g_callbacks = std::move(callbacks);
    }
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) {
      Forget();
      return false;
    }
    jvalue args[2];
    args[0].l = env->NewStringUTF(path.c_str());
    args[1].j = token;
    bool started = false;
    const bool called = CallActivity("playAudioFile", "(Ljava/lang/String;J)Z", args, &started);
    env->DeleteLocalRef(args[0].l);
    if (!called || !started) {
      Forget();
      return false;
    }
    return true;
  }

  void Pause() override { CallActivity("pauseAudioFile", "()V", nullptr, nullptr); }
  void Resume() override { CallActivity("resumeAudioFile", "()V", nullptr, nullptr); }
  void Stop() override {
    Forget();
    CallActivity("stopAudioFile", "()V", nullptr, nullptr);
  }

private:
  static void Forget() {
    std::lock_guard lock(g_mutex);
    g_token = 0;
    g_callbacks = {};
  }
};

} // namespace

std::unique_ptr<IAudioFilePlayer> CreateSystemAudioFilePlayer() {
  return std::make_unique<AndroidAudioFilePlayer>();
}

} // namespace pbr

/** PpAudioFilePlayer.nativeAudioFileProgress (the main thread). */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpAudioFilePlayer_nativeAudioFileProgress(
    JNIEnv*, jclass, jlong token, jint position_ms, jint duration_ms) {
  std::function<void(int64_t, int64_t)> on_progress;
  {
    std::lock_guard lock(pbr::g_mutex);
    if (token != pbr::g_token) {
      return;
    }
    on_progress = pbr::g_callbacks.on_progress;
  }
  if (on_progress) {
    on_progress(position_ms, duration_ms);
  }
}

/** PpAudioFilePlayer.nativeAudioFileEnded (the main thread): `finished` false = the file could not be decoded. */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpAudioFilePlayer_nativeAudioFileEnded(
    JNIEnv*, jclass, jlong token, jboolean finished) {
  std::function<void(pbr::VoicePlayEnd)> on_ended;
  {
    std::lock_guard lock(pbr::g_mutex);
    if (token != pbr::g_token) {
      return;
    }
    on_ended = std::move(pbr::g_callbacks.on_ended);
    pbr::g_token = 0;
    pbr::g_callbacks = {};
  }
  if (on_ended) {
    on_ended(finished ? pbr::VoicePlayEnd::Finished : pbr::VoicePlayEnd::Unplayable);
  }
}
