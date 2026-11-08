#include "foundation/platform/AndroidCallSystemUi.h"

#include "foundation/platform/CallSystemUi.h"

#include "common/runtime/AppRuntime.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

#include <deque>
#include <functional>
#include <mutex>
#include <string>
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
 * MainActivity's call-notification methods (they hand over to PpCallNotifications). The UI thread: SDL's, attached
 * to the VM; the activity's class is looked up from the activity, since FindClass on a native thread sees only the
 * system's classes.
 */
class Activity {
public:
  Activity() : env_(static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv())) {
    if (env_ != nullptr) {
      activity_ = static_cast<jobject>(SDL_GetAndroidActivity());
      if (activity_ != nullptr) {
        cls_ = env_->GetObjectClass(activity_);
      }
    }
  }
  ~Activity() {
    if (cls_ != nullptr) {
      env_->DeleteLocalRef(cls_);
    }
    if (activity_ != nullptr) {
      env_->DeleteLocalRef(activity_);
    }
  }
  Activity(const Activity&) = delete;
  Activity& operator=(const Activity&) = delete;

  bool CallBool(const char* method) {
    jmethodID mid = Method(method, "()Z");
    if (mid == nullptr) {
      return false;
    }
    const jboolean out = env_->CallBooleanMethod(activity_, mid);
    return !ClearedException() && out == JNI_TRUE;
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
  jmethodID Method(const char* name, const char* signature) {
    if (cls_ == nullptr) {
      return nullptr;
    }
    jmethodID mid = env_->GetMethodID(cls_, name, signature);
    if (mid == nullptr) {
      env_->ExceptionClear();
    }
    return mid;
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
  jclass cls_ = nullptr;
};

/**
 * ICallSystemUi on PpCallNotifications (UI thread). What the user does on the notification comes from Java on its
 * own threads, possibly before the app set its callbacks (Answer launched the app): it waits here until they are.
 */
class AndroidCallSystemUi final : public ICallSystemUi {
public:
  bool Available() const override { return Activity().CallBool("callNotificationsAvailable"); }

  void SetCallbacks(Callbacks callbacks) override {
    callbacks_ = std::move(callbacks);
    Deliver();
  }

  bool HasPendingPushCall() const override { return Activity().CallBool("hasPendingPushCall"); }
  void NotifyMessagingReady() override { Activity().CallVoid("callMessagingReady"); }
  void EndPendingPushCall(const EndReason reason) override {
    (void)reason; // the notification just goes
    Activity().CallVoid("endPendingPushCall");
  }
  void SetPendingChangedHandler(std::function<void()> fn) override { pending_changed_ = std::move(fn); }

  void ReportIncoming(const std::string& call_id, const std::string& caller_name, const bool video) override {
    Activity().CallVoid("showIncomingCall", call_id, caller_name, video);
  }

  void ReportEnded(const std::string& call_id, const EndReason reason) override {
    (void)reason;
    Activity().CallVoid("endIncomingCall", call_id);
  }

  /** Java, any thread. */
  void Queue(CallEvent event) {
    {
      std::lock_guard lock(mu_);
      events_.push_back(std::move(event));
    }
    AppRuntime::PostUI([this]() { Deliver(); });
  }

  /** Java, any thread. */
  void PendingChanged() {
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

  Callbacks callbacks_;
  std::function<void()> pending_changed_;
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

extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpCallNotifications_nativeOnPendingChanged(JNIEnv*, jclass) {
  pbr::Ui().PendingChanged();
}
