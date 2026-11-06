// Android: open a local file in an app that can show it. SDL_OpenURL hands Android a file:// address, which
// Android 7+ refuses for another app (FileUriExposedException, caught by SDL: "could not open"). MainActivity.openFile
// shares it through the app's FileProvider instead, with read permission for that one file and its MIME type.
#include "foundation/platform/PlatformOpenFile.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <jni.h>
#include <SDL3/SDL.h>

#include <filesystem>

namespace pbr {

bool PlatformOpenFile(const std::string& path) {
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec)) {
    return false;
  }
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool opened = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = cls != nullptr ? env->GetStaticMethodID(cls, "openFile", "(Ljava/lang/String;)Z") : nullptr;
  if (mid != nullptr) {
    jstring j_path = env->NewStringUTF(path.c_str());
    opened = env->CallStaticBooleanMethod(cls, mid, j_path) == JNI_TRUE;
    env->DeleteLocalRef(j_path);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    opened = false;
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  if (!opened) {
    logging::getLogger("attachment").warning << "attachment open: no app took it";
  }
  return opened;
}

void SetPlatformOpenFileSoundHooks(std::function<void()> /*begin*/, std::function<void()> /*end*/) {}

void PlatformForgetOpenedFiles() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()); // attaches a worker thread when it is not yet
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = cls != nullptr ? env->GetStaticMethodID(cls, "clearOpenLinks", "()V") : nullptr;
  if (mid != nullptr) {
    env->CallStaticVoidMethod(cls, mid);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    logging::getLogger("attachment").warning << "attachment open links: could not clear";
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
}

} // namespace pbr
