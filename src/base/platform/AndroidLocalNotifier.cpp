#include "base/platform/AndroidLocalNotifier.h"

#include "base/platform/AppLifecycle.h"

#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL.h>
#endif

namespace pbr {

void AndroidLocalNotifier::NotifyIncoming(const std::string& title, const std::string& body,
                                          const std::string& thread_id) {
  if (AppLifecycle::IsForeground()) {
    return;
  }
#if defined(__ANDROID__)
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return;
  }
  jmethodID mid =
      env->GetMethodID(cls, "showLocalNotification", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  jstring j_title = env->NewStringUTF(title.empty() ? "New message" : title.c_str());
  jstring j_body = env->NewStringUTF(body.empty() ? "You have a new message" : body.c_str());
  jstring j_thread = env->NewStringUTF(thread_id.c_str());
  env->CallVoidMethod(activity, mid, j_title, j_body, j_thread);
  env->DeleteLocalRef(j_title);
  env->DeleteLocalRef(j_body);
  env->DeleteLocalRef(j_thread);
  env->DeleteLocalRef(cls);
#else
  (void)title;
  (void)body;
  (void)thread_id;
#endif
}

void AndroidLocalNotifier::ClearForThread(const std::string& thread_id) {
#if defined(__ANDROID__)
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  if (cls == nullptr) {
    return;
  }
  jmethodID mid = env->GetMethodID(cls, "clearLocalNotification", "(Ljava/lang/String;)V");
  if (mid == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return;
  }
  jstring j_thread = env->NewStringUTF(thread_id.c_str());
  env->CallVoidMethod(activity, mid, j_thread);
  env->DeleteLocalRef(j_thread);
  env->DeleteLocalRef(cls);
#else
  (void)thread_id;
#endif
}

} // namespace pbr
