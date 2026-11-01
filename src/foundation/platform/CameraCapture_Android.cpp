#include "foundation/platform/CameraCapture.h"

#include <jni.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

namespace {

// Outcome codes shared with PpCameraCapture.java (OUTCOME_*).
constexpr int kOutcomePicked = 0;
constexpr int kOutcomeCancelled = 1;
constexpr int kOutcomeUnavailable = 2;
constexpr int kOutcomeUnprocessable = 3;

// Kind codes shared with PpCameraCapture.java (KIND_*).
constexpr int kKindPhoto = 0;
constexpr int kKindVideo = 1;

// The Java side delivers from its worker thread / the UI thread while ShowCameraCapture runs on the caller's thread,
// so the one pending callback is only touched under this mutex and always invoked outside it.
std::mutex g_mutex;
CameraCaptureCallback g_pending;
std::string g_scratch_dir;

/** Takes the pending callback (if any) and calls it once; a second delivery for the same capture finds nothing. */
void FinishPending(CameraCaptureOutcome outcome, std::vector<std::string> paths) {
  CameraCaptureCallback callback;
  {
    std::lock_guard lock(g_mutex);
    callback = std::move(g_pending);
    g_pending = nullptr;
  }
  if (callback) {
    callback(outcome, std::move(paths));
  }
}

/** Calls a static method of the running activity's class; false when the activity or method is not there. */
bool CallActivityStatic(const char* name, const char* signature, jvalue* args, jobject* object_result) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool ok = false;
  jclass cls = env->GetObjectClass(activity);
  if (cls != nullptr) {
    jmethodID mid = env->GetStaticMethodID(cls, name, signature);
    if (mid == nullptr) {
      env->ExceptionClear();
    } else if (object_result != nullptr) {
      *object_result = env->CallStaticObjectMethodA(cls, mid, args);
      ok = !env->ExceptionCheck();
    } else {
      env->CallStaticVoidMethodA(cls, mid, args);
      ok = !env->ExceptionCheck();
    }
    if (!ok) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return ok;
}

} // namespace

void ShowCameraCapture(SDL_Window* /*window*/, CameraCaptureKind kind, int max_video_seconds,
                       uint64_t max_video_bytes, CameraCaptureCallback callback) {
  if (!callback) {
    return;
  }
  bool busy = false;
  {
    std::lock_guard lock(g_mutex);
    if (g_pending) {
      busy = true;
    } else {
      g_pending = std::move(callback);
    }
  }
  if (busy) {
    callback(CameraCaptureOutcome::Cancelled, {});
    return;
  }
  // Android has one intent per kind; "either" opens the photo camera (the composer asks the user first).
  const int java_kind = kind == CameraCaptureKind::Video ? kKindVideo : kKindPhoto;
  jvalue args[3];
  args[0].i = java_kind;
  args[1].i = std::max(max_video_seconds, 0);
  args[2].j = static_cast<jlong>(std::min<uint64_t>(max_video_bytes, std::numeric_limits<jlong>::max()));
  if (!CallActivityStatic("startCameraCapture", "(IIJ)V", args, nullptr)) {
    FinishPending(CameraCaptureOutcome::Unavailable, {});
  }
}

std::string CaptureScratchDir() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_scratch_dir.empty()) {
      return g_scratch_dir;
    }
  }
  jobject j_dir = nullptr;
  if (!CallActivityStatic("captureScratchDir", "()Ljava/lang/String;", nullptr, &j_dir) || j_dir == nullptr) {
    return {};
  }
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  std::string dir;
  if (env != nullptr) {
    const char* chars = env->GetStringUTFChars(static_cast<jstring>(j_dir), nullptr);
    if (chars != nullptr) {
      dir = chars;
      env->ReleaseStringUTFChars(static_cast<jstring>(j_dir), chars);
    }
    env->DeleteLocalRef(j_dir);
  }
  if (!dir.empty()) {
    std::lock_guard lock(g_mutex);
    g_scratch_dir = dir;
  }
  return dir;
}

} // namespace pbr

/** PpCameraCapture.nativeCameraCaptureResult: the camera flow ended (any thread). */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpCameraCapture_nativeCameraCaptureResult(
    JNIEnv* env, jclass, jint outcome, jstring path) {
  std::string file;
  if (path != nullptr) {
    const char* chars = env->GetStringUTFChars(path, nullptr);
    if (chars != nullptr) {
      file = chars;
      env->ReleaseStringUTFChars(path, chars);
    }
  }
  switch (outcome) {
    case pbr::kOutcomePicked:
      if (file.empty()) {
        pbr::FinishPending(pbr::CameraCaptureOutcome::Unavailable, {});
      } else {
        pbr::FinishPending(pbr::CameraCaptureOutcome::Picked, {std::move(file)});
      }
      break;
    case pbr::kOutcomeCancelled:
      pbr::FinishPending(pbr::CameraCaptureOutcome::Cancelled, {});
      break;
    case pbr::kOutcomeUnprocessable:
      pbr::FinishPending(pbr::CameraCaptureOutcome::Unprocessable, {});
      break;
    case pbr::kOutcomeUnavailable:
    default:
      pbr::FinishPending(pbr::CameraCaptureOutcome::Unavailable, {});
      break;
  }
}
