#include "foundation/platform/IncomingLink.h"

#include "foundation/runtime/AppRuntime.h"

#include <cctype>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace pbr {

namespace {

std::mutex g_mutex;
std::function<void(std::string)> g_handler;
std::vector<std::string> g_waiting;

void PostToHandler(std::function<void(std::string)> handler, std::string link) {
  AppRuntime::PostUI([handler = std::move(handler), link = std::move(link)]() { handler(link); });
}

} // namespace

bool IncomingLink::LooksLikeLink(const std::string_view text) {
  const size_t colon = text.find(':');
  if (colon == std::string_view::npos || colon < 2) {
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(text[0]))) {
    return false;
  }
  for (size_t i = 1; i < colon; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

void IncomingLink::Deliver(std::string link) {
  std::function<void(std::string)> handler;
  {
    std::lock_guard lock(g_mutex);
    if (!g_handler) {
      g_waiting.push_back(std::move(link));
      return;
    }
    handler = g_handler;
  }
  PostToHandler(std::move(handler), std::move(link));
}

void IncomingLink::SetHandler(std::function<void(std::string link)> handler) {
  std::vector<std::string> waiting;
  {
    std::lock_guard lock(g_mutex);
    g_handler = handler;
    if (handler) {
      waiting.swap(g_waiting);
    }
  }
  for (std::string& link : waiting) {
    PostToHandler(handler, std::move(link));
  }
}

} // namespace pbr

#if defined(__ANDROID__)
/** PpLinkBridge.nativeOnOpenLink: the activity's VIEW intent for a scheme the app registered. */
extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpLinkBridge_nativeOnOpenLink(JNIEnv* env, jclass,
                                                                                         jstring link) {
  if (!link) {
    return;
  }
  const char* chars = env->GetStringUTFChars(link, nullptr);
  if (!chars) {
    return;
  }
  std::string value(chars);
  env->ReleaseStringUTFChars(link, chars);
  pbr::IncomingLink::Deliver(std::move(value));
}
#endif
