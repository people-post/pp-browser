#include "foundation/platform/os/OsDeviceName.h"

#include "common/DeviceName.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <cstdint>

namespace pbr::os {

namespace {

// JNI's GetStringUTFChars gives modified UTF-8 (an emoji as two 3-byte surrogates), which NormalizeDeviceName
// rejects: take the UTF-16 units and write real UTF-8. A lone surrogate gives "" (like any invalid input).
std::string Utf8FromUtf16(const jchar* units, const jsize count) {
  std::string out;
  for (jsize i = 0; i < count; ++i) {
    uint32_t cp = units[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      return {};
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

} // namespace

// MainActivity.deviceName(): Settings.Global.DEVICE_NAME, else the maker and model. Main thread only (startup).
std::string SystemDeviceName() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return {};
  }
  std::string raw;
  if (jclass cls = env->GetObjectClass(activity); cls != nullptr) {
    if (jmethodID mid = env->GetStaticMethodID(cls, "deviceName", "()Ljava/lang/String;"); mid != nullptr) {
      auto jname = static_cast<jstring>(env->CallStaticObjectMethod(cls, mid));
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      } else if (jname != nullptr) {
        if (const jchar* units = env->GetStringChars(jname, nullptr)) {
          raw = Utf8FromUtf16(units, env->GetStringLength(jname));
          env->ReleaseStringChars(jname, units);
        }
        env->DeleteLocalRef(jname);
      }
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return NormalizeDeviceName(raw);
}

} // namespace pbr::os
