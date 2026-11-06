// Android: a sound file's title, artist, length and cover through android.media.MediaMetadataRetriever (ID3 TIT2 /
// TPE1 / APIC in an MP3, the iTunes atoms of an M4A), called over JNI from the worker that asks.
#include "lib/voice/AudioFileTags.h"

#include <jni.h>
#include <SDL3/SDL.h>

#include <cstdlib>

namespace pbr {
namespace {

// MediaMetadataRetriever.METADATA_KEY_*.
constexpr jint kKeyArtist = 2;
constexpr jint kKeyTitle = 7;
constexpr jint kKeyDuration = 9;

bool Failed(JNIEnv* env) {
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return true;
  }
  return false;
}

std::string Extract(JNIEnv* env, jobject retriever, jmethodID extract, const jint key) {
  auto* value = static_cast<jstring>(env->CallObjectMethod(retriever, extract, key));
  if (Failed(env) || value == nullptr) {
    return {};
  }
  std::string text;
  if (const char* chars = env->GetStringUTFChars(value, nullptr)) {
    text = chars;
    env->ReleaseStringUTFChars(value, chars);
  }
  env->DeleteLocalRef(value);
  return text;
}

} // namespace

std::optional<AudioFileTags> ReadAudioFileTags(const std::string& path) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()); // attaches this worker thread when it is not yet
  if (env == nullptr || env->PushLocalFrame(16) != 0) {
    return std::nullopt;
  }
  std::optional<AudioFileTags> result;
  jclass cls = env->FindClass("android/media/MediaMetadataRetriever");
  jmethodID init = cls != nullptr ? env->GetMethodID(cls, "<init>", "()V") : nullptr;
  jmethodID set_source = cls != nullptr ? env->GetMethodID(cls, "setDataSource", "(Ljava/lang/String;)V") : nullptr;
  jmethodID extract = cls != nullptr ? env->GetMethodID(cls, "extractMetadata", "(I)Ljava/lang/String;") : nullptr;
  jmethodID picture = cls != nullptr ? env->GetMethodID(cls, "getEmbeddedPicture", "()[B") : nullptr;
  jmethodID release = cls != nullptr ? env->GetMethodID(cls, "release", "()V") : nullptr;
  jobject retriever = !Failed(env) && init != nullptr ? env->NewObject(cls, init) : nullptr;
  if (retriever != nullptr && !Failed(env) && set_source && extract && picture && release) {
    env->CallVoidMethod(retriever, set_source, env->NewStringUTF(path.c_str()));
    if (!Failed(env)) {
      AudioFileTags tags;
      tags.title = Extract(env, retriever, extract, kKeyTitle);
      tags.artist = Extract(env, retriever, extract, kKeyArtist);
      const std::string duration = Extract(env, retriever, extract, kKeyDuration);
      tags.duration_ms = duration.empty() ? 0 : std::strtoll(duration.c_str(), nullptr, 10);
      auto* bytes = static_cast<jbyteArray>(env->CallObjectMethod(retriever, picture));
      if (!Failed(env) && bytes != nullptr) {
        const jsize length = env->GetArrayLength(bytes);
        if (length > 0 && static_cast<size_t>(length) <= kMaxAudioCoverBytes) {
          tags.picture.resize(static_cast<size_t>(length));
          env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(tags.picture.data()));
          if (Failed(env)) {
            tags.picture.clear();
          }
        }
      }
      result = std::move(tags);
    }
    env->CallVoidMethod(retriever, release);
    Failed(env);
  }
  Failed(env);
  env->PopLocalFrame(nullptr);
  return result;
}

} // namespace pbr
