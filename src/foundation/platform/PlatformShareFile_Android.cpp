// Android: a received attachment through the system share sheet. Like PlatformOpenFile_Android.cpp, a file:// address
// is refused by Android 7+: MainActivity.shareFile hands it over as a FileProvider address (ACTION_SEND) with read
// permission for that one file.
#include "foundation/platform/PlatformShareFile.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <jni.h>
#include <SDL3/SDL.h>

#include <filesystem>

namespace pbr {

bool PlatformCanShareFile() {
  return true;
}

bool PlatformShareFile(SDL_Window* /*window*/, const std::string& path, const std::string& mime,
                       ShareAnchor /*anchor*/) {
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec)) {
    return false;
  }
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  bool shared = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid =
      cls != nullptr ? env->GetStaticMethodID(cls, "shareFile", "(Ljava/lang/String;Ljava/lang/String;)Z") : nullptr;
  if (mid != nullptr) {
    jstring j_path = env->NewStringUTF(path.c_str());
    jstring j_mime = env->NewStringUTF(mime.c_str());
    shared = env->CallStaticBooleanMethod(cls, mid, j_path, j_mime) == JNI_TRUE;
    env->DeleteLocalRef(j_mime);
    env->DeleteLocalRef(j_path);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    shared = false;
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  if (!shared) {
    logging::getLogger("attachment").warning << "attachment share: no sheet";
  }
  return shared;
}

} // namespace pbr
