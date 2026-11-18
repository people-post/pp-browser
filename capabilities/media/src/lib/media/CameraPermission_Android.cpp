#if defined(__ANDROID__)

#include "lib/media/CameraPermission.h"

#include <jni.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <atomic>
#include <memory>

namespace pbr {
namespace {

constexpr const char* kCamera = "android.permission.CAMERA";

/** Set once the person has answered our prompt with "no": Android may not show it again, so this is a refusal. */
std::atomic<bool> g_refused{false};

/** Context.checkSelfPermission through the activity: 0 = granted. */
bool IsGranted() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  // SDL hands out a new local reference each time; this runs on SDL's native thread, whose JNI frame never pops.
  bool granted = false;
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    jmethodID mid = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (mid != nullptr) {
      jstring name = env->NewStringUTF(kCamera);
      granted = env->CallIntMethod(activity, mid, name) == 0;
      env->DeleteLocalRef(name);
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
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

// A refusal made in an earlier run reads NotDetermined here; asking again then answers "no" at once when Android
// will not show its prompt any more, so the caller still ends at its "allow it in Settings" hint.
CameraPermission CameraPermissionState() {
  if (IsGranted()) {
    return CameraPermission::Granted;
  }
  return g_refused.load() ? CameraPermission::Denied : CameraPermission::NotDetermined;
}

void RequestCameraPermission(std::function<void(bool granted)> done) {
  auto* pending = new Pending{std::move(done)};
  if (!SDL_RequestAndroidPermission(kCamera, OnAnswer, pending)) {
    std::unique_ptr<Pending> own(pending);
    if (own->done) {
      own->done(false);
    }
  }
}

} // namespace pbr

#endif
