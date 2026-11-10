#include "foundation/platform/AndroidCallSystemUi.h"

#include "foundation/platform/CallSystemUi.h"

#include "common/runtime/AppRuntime.h"
#include "foundation/runtime/AppLifecycle.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace pbr {

namespace {

/** An answer, an end or the push's call gone, from Java (any thread), for the UI thread. */
struct CallEvent {
  enum class Kind { Answer, End, PendingGone };
  Kind kind = Kind::Answer;
  std::string call_id;
};

/**
 * MainActivity's call-notification methods (they hand over to PpCallNotifications). The UI thread only: SDL's,
 * attached to the VM. The activity's class is looked up once from the activity (FindClass on a native thread sees
 * only the system's classes) and kept, with each method's id, for the process: neither changes while it runs.
 */
class Activity {
public:
  Activity() : env_(static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv())) {
    if (env_ != nullptr) {
      activity_ = static_cast<jobject>(SDL_GetAndroidActivity());
    }
  }
  ~Activity() {
    if (activity_ != nullptr) {
      env_->DeleteLocalRef(activity_);
    }
  }
  Activity(const Activity&) = delete;
  Activity& operator=(const Activity&) = delete;

  /** Nothing: Java could not be asked (no activity, no such method, it threw). */
  std::optional<bool> CallBool(const char* method) {
    jmethodID mid = Method(method, "()Z");
    if (mid == nullptr) {
      return std::nullopt;
    }
    const jboolean out = env_->CallBooleanMethod(activity_, mid);
    if (ClearedException()) {
      return std::nullopt;
    }
    return out == JNI_TRUE;
  }

  void CallVoid(const char* method) {
    if (jmethodID mid = Method(method, "()V"); mid != nullptr) {
      env_->CallVoidMethod(activity_, mid);
      ClearedException();
    }
  }

  void CallVoid(const char* method, const std::string& call_id) {
    if (jmethodID mid = Method(method, "(Ljava/lang/String;)V"); mid != nullptr) {
      jstring j_id = env_->NewStringUTF(call_id.c_str());
      env_->CallVoidMethod(activity_, mid, j_id);
      ClearedException();
      env_->DeleteLocalRef(j_id);
    }
  }

  void CallVoid(const char* method, const std::string& call_id, const std::string& name, const bool video) {
    if (jmethodID mid = Method(method, "(Ljava/lang/String;Ljava/lang/String;Z)V"); mid != nullptr) {
      jstring j_id = env_->NewStringUTF(call_id.c_str());
      jstring j_name = env_->NewStringUTF(name.c_str());
      env_->CallVoidMethod(activity_, mid, j_id, j_name, static_cast<jboolean>(video ? JNI_TRUE : JNI_FALSE));
      ClearedException();
      env_->DeleteLocalRef(j_name);
      env_->DeleteLocalRef(j_id);
    }
  }

private:
  /** The method's id, looked up the first time (UI thread: no lock). Null: no such method (or no activity). */
  jmethodID Method(const char* name, const char* signature) {
    if (activity_ == nullptr) {
      return nullptr;
    }
    static std::unordered_map<std::string, jmethodID> ids;
    const std::string key = std::string(name) + signature;
    if (const auto it = ids.find(key); it != ids.end()) {
      return it->second;
    }
    jclass cls = Class();
    if (cls == nullptr) {
      return nullptr;
    }
    jmethodID mid = env_->GetMethodID(cls, name, signature);
    if (mid == nullptr) {
      env_->ExceptionClear();
      return nullptr; // not kept: a missing method is a build mismatch, and asking again costs only that call
    }
    ids.emplace(key, mid);
    return mid;
  }

  /** The activity's class, as a global reference kept for the process. */
  jclass Class() {
    static jclass cls = nullptr;
    if (cls == nullptr) {
      jclass local = env_->GetObjectClass(activity_);
      if (local == nullptr) {
        return nullptr;
      }
      cls = static_cast<jclass>(env_->NewGlobalRef(local));
      env_->DeleteLocalRef(local);
    }
    return cls;
  }

  bool ClearedException() {
    if (!env_->ExceptionCheck()) {
      return false;
    }
    env_->ExceptionClear();
    return true;
  }

  JNIEnv* env_ = nullptr;
  jobject activity_ = nullptr;
};

/**
 * ICallSystemUi on PpCallNotifications (UI thread). What the user does on the notification comes from Java on its
 * own threads, possibly before the app set its callbacks (Answer launched the app): it waits here until they are.
 *
 * The bridge reads Available() and HasPendingPushCall() every frame of a call, and on Java each is a call into the
 * activity (availability also asks the notification service): both are kept here. Availability is asked again on
 * RecheckAvailable (a new ring, the app back in front); the pending flag comes with each change from Java
 * (nativeOnPendingChanged), read from Java only once, before the first change seen.
 */
class AndroidCallSystemUi final : public ICallSystemUi {
public:
  bool Available() const override {
    if (const int known = available_.load(); known != kUnknown) {
      return known != 0;
    }
    const std::optional<bool> asked = Activity().CallBool("callNotificationsAvailable");
    if (asked) {
      available_.store(*asked ? 1 : 0); // not kept when Java could not answer: asked again next time
    }
    return asked.value_or(false);
  }

  void RecheckAvailable() override { available_.store(kUnknown); }

  void SetCallbacks(Callbacks callbacks) override {
    callbacks_ = std::move(callbacks);
    Deliver();
  }

  bool HasPendingPushCall() const override {
    if (const int known = pending_.load(); known != kUnknown) {
      return known != 0;
    }
    const std::optional<bool> asked = Activity().CallBool("hasPendingPushCall");
    if (!asked) {
      return false;
    }
    // A change Java sent meanwhile is newer than this read: it stays.
    int expected = kUnknown;
    return pending_.compare_exchange_strong(expected, *asked ? 1 : 0) ? *asked : expected != 0;
  }
  void NotifyMessagingReady() override { Activity().CallVoid("callMessagingReady"); }
  void EndPendingPushCall() override { Activity().CallVoid("endPendingPushCall"); }
  void SetPendingChangedHandler(std::function<void()> fn) override { pending_changed_ = std::move(fn); }

  void ReportIncoming(const std::string& call_id, const std::string& caller_name, const bool video) override {
    Activity().CallVoid("showIncomingCall", call_id, caller_name, video);
  }

  void ReportEnded(const std::string& call_id) override { Activity().CallVoid("endIncomingCall", call_id); }

  /** Java, any thread. */
  void Queue(CallEvent event) {
    {
      std::lock_guard lock(mu_);
      events_.push_back(std::move(event));
    }
    AppRuntime::PostUI([this]() { Deliver(); });
  }

  /** Java, any thread (under PpCallNotifications' lock, so in the order of the changes). */
  void PendingChanged(const bool pending) {
    pending_.store(pending ? 1 : 0);
    AppRuntime::PostUI([this]() {
      if (pending_changed_) {
        pending_changed_();
      }
    });
  }

private:
  /** UI thread: the events so far, in order, once the callbacks are set. */
  void Deliver() {
    if (!callbacks_.answer) {
      return;
    }
    std::deque<CallEvent> events;
    {
      std::lock_guard lock(mu_);
      events.swap(events_);
    }
    for (const CallEvent& event : events) {
      switch (event.kind) {
        case CallEvent::Kind::Answer:
          Run(callbacks_.answer, event.call_id);
          break;
        case CallEvent::Kind::End:
          Run(callbacks_.end, event.call_id);
          break;
        case CallEvent::Kind::PendingGone:
          if (callbacks_.pending_gone) {
            callbacks_.pending_gone();
          }
          break;
      }
    }
  }

  static void Run(const std::function<void(const std::string&)>& fn, const std::string& call_id) {
    if (fn) {
      fn(call_id);
    }
  }

  static constexpr int kUnknown = -1;

  Callbacks callbacks_;
  std::function<void()> pending_changed_;
  /** 1 / 0, kUnknown: not asked yet (Available: or asked to again). */
  mutable std::atomic<int> available_{kUnknown};
  mutable std::atomic<int> pending_{kUnknown};
  std::mutex mu_;
  std::deque<CallEvent> events_;
};

AndroidCallSystemUi& Ui() {
  static AndroidCallSystemUi ui;
  return ui;
}

std::string StringOf(JNIEnv* env, jstring s) {
  if (env == nullptr || s == nullptr) {
    return {};
  }
  const char* utf = env->GetStringUTFChars(s, nullptr);
  if (utf == nullptr) {
    return {};
  }
  std::string out = utf;
  env->ReleaseStringUTFChars(s, utf);
  return out;
}

} // namespace

void InstallAndroidCallSystemUi() {
  ICallSystemUi::SetInstance(&Ui());
  // Back from the settings, perhaps with call notifications turned off or on (UI thread).
  static const AppLifecycle::ListenerId foreground =
      AppLifecycle::AddForegroundListener([]() { Ui().RecheckAvailable(); });
  (void)foreground;
}

} // namespace pbr

extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpCallNotifications_nativeOnAnswer(JNIEnv* env, jclass,
                                                                                              jstring call_id) {
  pbr::Ui().Queue({pbr::CallEvent::Kind::Answer, pbr::StringOf(env, call_id)});
}

extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpCallNotifications_nativeOnEnd(JNIEnv* env, jclass,
                                                                                           jstring call_id) {
  pbr::Ui().Queue({pbr::CallEvent::Kind::End, pbr::StringOf(env, call_id)});
}

extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpCallNotifications_nativeOnPendingGone(JNIEnv*, jclass) {
  pbr::Ui().Queue({pbr::CallEvent::Kind::PendingGone, {}});
}

extern "C" JNIEXPORT void JNICALL
Java_dev_pp_1browser_app_PpCallNotifications_nativeOnPendingChanged(JNIEnv*, jclass, jboolean pending) {
  pbr::Ui().PendingChanged(pending == JNI_TRUE);
}
