// Android: ConnectivityManager default-network callback (PpNetworkMonitor.java) → JNI.
// MainActivity starts / stops it; every callback reports the whole state.

#include "foundation/platform/NetworkMonitorBackend.h"

#include <jni.h>
#include <SDL3/SDL.h>

#include <mutex>
#include <string>

namespace pbr::detail {
namespace {

std::mutex g_sink_mu;
INetworkMonitorBackend::StateSink g_sink;

/** Calls a `()V` method on the SDL activity. */
bool CallActivity(const char* method) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return false;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID mid = cls ? env->GetMethodID(cls, method, "()V") : nullptr;
  bool ok = false;
  if (mid != nullptr) {
    env->CallVoidMethod(activity, mid);
    ok = !env->ExceptionCheck();
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  return ok;
}

class AndroidNetworkMonitorBackend final : public INetworkMonitorBackend {
public:
  ~AndroidNetworkMonitorBackend() override { Stop(); }

  bool Start(StateSink sink) override {
    {
      std::lock_guard lock(g_sink_mu);
      g_sink = std::move(sink);
    }
    started_ = CallActivity("startNetworkMonitor");
    return started_;
  }

  void Stop() override {
    if (started_) {
      (void)CallActivity("stopNetworkMonitor");
      started_ = false;
    }
    std::lock_guard lock(g_sink_mu);  // waits out a report in flight
    g_sink = nullptr;
  }

private:
  bool started_ = false;
};

} // namespace

std::unique_ptr<INetworkMonitorBackend> CreateNetworkMonitorBackend() {
  return std::make_unique<AndroidNetworkMonitorBackend>();
}

} // namespace pbr::detail

extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpNetworkMonitor_nativeOnNetworkState(
    JNIEnv* env, jclass, jboolean online, jint transport, jboolean expensive, jstring fingerprint) {
  pbr::NetworkState state;
  state.online = online == JNI_TRUE;
  switch (transport) {  // PpNetworkMonitor.TRANSPORT_*
  case 1:
    state.transport = pbr::NetworkTransport::Wifi;
    break;
  case 2:
    state.transport = pbr::NetworkTransport::Cellular;
    break;
  case 3:
    state.transport = pbr::NetworkTransport::Other;
    break;
  default:
    state.transport = pbr::NetworkTransport::Unknown;
    break;
  }
  state.expensive = expensive == JNI_TRUE;
  if (fingerprint != nullptr) {
    if (const char* chars = env->GetStringUTFChars(fingerprint, nullptr)) {
      state.fingerprint = chars;
      env->ReleaseStringUTFChars(fingerprint, chars);
    }
  }
  std::lock_guard lock(pbr::detail::g_sink_mu);
  if (pbr::detail::g_sink) {
    pbr::detail::g_sink(state);
  }
}
