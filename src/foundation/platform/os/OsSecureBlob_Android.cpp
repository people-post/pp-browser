#include "foundation/platform/os/OsSecureBlob.h"

#include <SDL3/SDL.h>
#include <android/api-level.h>
#include <jni.h>

#include <string>
#include <vector>

namespace pbr::os {

namespace {

// PpSecureBlob.java does the work (AES-256-GCM under an AndroidKeyStore key, the ciphertext in
// noBackupFilesDir/secure_blob: never backed up, gone with an uninstall), reached through MainActivity's statics:
// FindClass from a native thread does not see the app's classes. API 28 is the first with
// setUnlockedDeviceRequired (kWhenUnlocked); below it every call is the Default stub's.
constexpr int kFirstApi = 28;

// Byte 0 of PpSecureBlob.lookup's answer (PpSecureBlob.FOUND / MISSING / UNAVAILABLE; anything else is ERROR).
constexpr uint8_t kFound = 0;
constexpr uint8_t kMissing = 1;
constexpr uint8_t kUnavailable = 2;

/** The blob crossed through Java: no copy of it stays in a byte[] the GC may keep around. */
void WipeJava(JNIEnv* env, jbyteArray array) {
  const jsize length = env->GetArrayLength(array);
  if (length > 0) {
    const std::vector<jbyte> zeros(static_cast<size_t>(length), 0);
    env->SetByteArrayRegion(array, 0, length, zeros.data());
  }
}

void Wipe(std::vector<uint8_t>& bytes) {
  volatile uint8_t* p = bytes.data();
  for (size_t i = 0; i < bytes.size(); ++i) {
    p[i] = 0;
  }
}

jbyteArray ToJava(JNIEnv* env, const uint8_t* data, const size_t size) {
  jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
  if (array != nullptr && size > 0) {
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(data));
  }
  return array;
}

std::string FromJava(JNIEnv* env, jstring text) {
  std::string out;
  if (const char* utf = env->GetStringUTFChars(text, nullptr)) {
    out = utf;
    env->ReleaseStringUTFChars(text, utf);
  }
  return out;
}

/**
 * Calls a static method of the running activity's class (MainActivity) that returns an object. False when there is
 * no activity or method, or it threw; `*result` is then null, else a local ref (null too when Java returned null).
 */
bool CallActivityObject(JNIEnv* env, const char* name, const char* signature, const jvalue* args, jobject* result) {
  *result = nullptr;
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (activity == nullptr) {
    return false;
  }
  bool ok = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = cls != nullptr ? env->GetStaticMethodID(cls, name, signature) : nullptr;
  if (mid != nullptr) {
    *result = env->CallStaticObjectMethodA(cls, mid, args);
    ok = !env->ExceptionCheck();
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    if (*result != nullptr) {
      env->DeleteLocalRef(*result);
    }
    *result = nullptr;
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return ok;
}

/** The name as Java sees it (bytes, so hashing and the AAD never depend on a string encoding). */
jbyteArray NameToJava(JNIEnv* env, const std::string& name) {
  return ToJava(env, reinterpret_cast<const uint8_t*>(name.data()), name.size());
}

/** A Java call that answers null on success, else what failed. */
Roe<void> CallReturningFailure(JNIEnv* env, const char* method, const char* signature, const jvalue* args) {
  jobject failure = nullptr;
  if (!CallActivityObject(env, method, signature, args, &failure)) {
    return Error(std::string("Secure blob: MainActivity.") + method + " could not be called");
  }
  if (failure != nullptr) {
    std::string message = FromJava(env, static_cast<jstring>(failure));
    env->DeleteLocalRef(failure);
    return Error(std::string("Secure blob: ") + method + ": " + message);
  }
  return {};
}

} // namespace

Roe<void> SecureBlobWrite(const std::string& name, const std::vector<uint8_t>& bytes) {
  return SecureBlobWriteWithAccess(name, bytes, SecureBlobAccess::kAfterFirstUnlock);
}

Roe<std::optional<std::vector<uint8_t>>> SecureBlobRead(const std::string& name) {
  auto looked = SecureBlobLookup(name);
  if (!looked) {
    return looked.error();
  }
  switch (looked->status) {
  case pbr::SecureBlobLookup::Status::Found:
    return std::optional<std::vector<uint8_t>>(std::move(looked->bytes));
  case pbr::SecureBlobLookup::Status::Missing:
    return std::optional<std::vector<uint8_t>>{};
  case pbr::SecureBlobLookup::Status::Unavailable:
  case pbr::SecureBlobLookup::Status::Canceled:  // no prompts on Android yet (#137)
    break;
  }
  return Error("Secure blob: not readable now (code " + std::to_string(looked->code) + ")");
}

Roe<void> SecureBlobDelete(const std::string& name) {
  if (!SecureBlobSupported()) {
    return {};
  }
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) {
    return Error("Secure blob: no JNI environment");
  }
  jvalue args[1];
  args[0].l = NameToJava(env, name);
  auto deleted = CallReturningFailure(env, "secureBlobDelete", "([B)Ljava/lang/String;", args);
  env->DeleteLocalRef(args[0].l);
  return deleted;
}

Roe<void> SecureBlobWriteWithAccess(const std::string& name, const std::vector<uint8_t>& bytes,
                                    const SecureBlobAccess access) {
  if (!SecureBlobSupported()) {
    return Error("Secure blob store is not supported on this platform");
  }
  if (access == SecureBlobAccess::kBiometryCurrentSet) {
    return Error("Biometry-bound secure blobs are not supported on Android yet");  // fingerprint unlock: #137
  }
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) {
    return Error("Secure blob: no JNI environment");
  }
  // One file per name whatever the level: a write under another level replaces it (the iOS delete-and-add), and
  // the old file stays until the new one is complete.
  jvalue args[3];
  args[0].l = NameToJava(env, name);
  args[1].l = ToJava(env, bytes.data(), bytes.size());
  args[2].z = access == SecureBlobAccess::kWhenUnlocked ? JNI_TRUE : JNI_FALSE;
  auto written = CallReturningFailure(env, "secureBlobWrite", "([B[BZ)Ljava/lang/String;", args);
  if (args[1].l != nullptr) {
    WipeJava(env, static_cast<jbyteArray>(args[1].l));  // Java wipes it too; not when the call never got there
    env->DeleteLocalRef(args[1].l);
  }
  env->DeleteLocalRef(args[0].l);
  return written;
}

Roe<pbr::SecureBlobLookup> SecureBlobLookup(const std::string& name) {
  pbr::SecureBlobLookup out;
  if (!SecureBlobSupported()) {
    return out; // Missing, as the Default stub
  }
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) {
    return Error("Secure blob: no JNI environment");
  }
  jvalue args[1];
  args[0].l = NameToJava(env, name);
  jobject answer = nullptr;
  const bool called = CallActivityObject(env, "secureBlobLookup", "([B)[B", args, &answer);
  env->DeleteLocalRef(args[0].l);
  if (!called || answer == nullptr) {
    return Error("Secure blob: MainActivity.secureBlobLookup could not be called");
  }
  jbyteArray array = static_cast<jbyteArray>(answer);
  std::vector<uint8_t> bytes(static_cast<size_t>(env->GetArrayLength(array)));
  if (!bytes.empty()) {
    env->GetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<jbyte*>(bytes.data()));
  }
  WipeJava(env, array);
  env->DeleteLocalRef(answer);
  if (bytes.empty()) {
    return Error("Secure blob: empty lookup answer");
  }
  // Java says Missing only when the blob can never open again (no file, no key, the key invalidated, a bad tag):
  // GraceUnlock deletes on Missing. Locked → Unavailable with PpSecureBlob's code (1 lock screen up, 2 the Keystore
  // refused); anything else is an error, and errors keep the blob too.
  switch (bytes[0]) {
  case kFound:
    out.status = pbr::SecureBlobLookup::Status::Found;
    out.bytes.assign(bytes.begin() + 1, bytes.end());
    Wipe(bytes);
    return out;
  case kMissing:
    out.status = pbr::SecureBlobLookup::Status::Missing;
    return out;
  case kUnavailable:
    out.status = pbr::SecureBlobLookup::Status::Unavailable;
    out.code = bytes.size() > 1 ? bytes[1] : 0;
    return out;
  default:
    return Error("Secure blob: " + std::string(bytes.begin() + 1, bytes.end()));
  }
}

// No biometry-bound entries on Android yet (#137): a prompt has nothing to guard, the plain lookup answers.
Roe<pbr::SecureBlobLookup> SecureBlobLookupWithPrompt(const std::string& name, const std::string& /*prompt*/) {
  return SecureBlobLookup(name);
}

bool SecureBlobSupported() {
  return android_get_device_api_level() >= kFirstApi;
}

} // namespace pbr::os
