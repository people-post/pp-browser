#include "foundation/platform/AndroidLocalNotifier.h"

#include "foundation/runtime/AppLifecycle.h"

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
  // SDL hands out a new local reference each time; on SDL's native thread the JNI frame never pops.
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    jmethodID mid =
        env->GetMethodID(cls, "showLocalNotification", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (mid != nullptr) {
      jstring j_title = env->NewStringUTF(title.empty() ? "New message" : title.c_str());
      jstring j_body = env->NewStringUTF(body.empty() ? "You have a new message" : body.c_str());
      jstring j_thread = env->NewStringUTF(thread_id.c_str());
      env->CallVoidMethod(activity, mid, j_title, j_body, j_thread);
      env->DeleteLocalRef(j_title);
      env->DeleteLocalRef(j_body);
      env->DeleteLocalRef(j_thread);
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
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
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    jmethodID mid = env->GetMethodID(cls, "clearLocalNotification", "(Ljava/lang/String;)V");
    if (mid != nullptr) {
      jstring j_thread = env->NewStringUTF(thread_id.c_str());
      env->CallVoidMethod(activity, mid, j_thread);
      env->DeleteLocalRef(j_thread);
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
#else
  (void)thread_id;
#endif
}

void AndroidLocalNotifier::SetMessageBannersOn(const bool on) {
#if defined(__ANDROID__)
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetMethodID(cls, "setMessageBannersOn", "(Z)V"); mid != nullptr) {
      env->CallVoidMethod(activity, mid, static_cast<jboolean>(on ? JNI_TRUE : JNI_FALSE));
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
#else
  (void)on;
#endif
}

} // namespace pbr
