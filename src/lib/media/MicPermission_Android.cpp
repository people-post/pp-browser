#if defined(__ANDROID__)

#include "lib/media/MicPermission.h"

#include <jni.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <atomic>
#include <memory>

namespace pbr {
namespace {

constexpr const char* kRecordAudio = "android.permission.RECORD_AUDIO";

/** Set once the person has answered our prompt with "no": Android will not show it again, so this is a refusal. */
std::atomic<bool> g_refused{false};

/** Context.checkSelfPermission through the activity: 0 = granted. */
bool IsGranted() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return false;
  }
  jmethodID mid = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
  bool granted = false;
  if (mid != nullptr) {
    jstring name = env->NewStringUTF(kRecordAudio);
    granted = env->CallIntMethod(activity, mid, name) == 0;
    env->DeleteLocalRef(name);
  } else {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(cls);
  return granted;
}

struct Pending {
  std::function<void(bool)> done;
};

void SDLCALL OnAnswer(void* userdata, const char* /*permission*/, bool granted) {
  std::unique_ptr<Pending> pending(static_cast<Pending*>(userdata));
  g_refused.store(!granted);
  if (pending->done) {
    pending->done(granted);
  }
}

} // namespace

MicPermission MicPermissionState() {
  if (IsGranted()) {
    return MicPermission::Granted;
  }
  return g_refused.load() ? MicPermission::Denied : MicPermission::NotDetermined;
}

void RequestMicPermission(std::function<void(bool granted)> done) {
  auto* pending = new Pending{std::move(done)};
  if (!SDL_RequestAndroidPermission(kRecordAudio, OnAnswer, pending)) {
    std::unique_ptr<Pending> own(pending);
    if (own->done) {
      own->done(false);
    }
  }
}

} // namespace pbr

#endif
