#include "base/platform/PlatformLogSink.h"

#include "base/runtime/ProductBranding.h"
#include "common/Logger.h"

#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <SDL3/SDL.h>
#include <os/log.h>
#endif
#endif

namespace pbr {

namespace {

#if defined(__APPLE__) && TARGET_OS_IPHONE
std::mutex g_file_mu;
FILE* g_log_file = nullptr;

void EnsureIosLogFile() {
  if (g_log_file) {
    return;
  }
  if (char* pref = SDL_GetPrefPath("dev.pp-browser", "pp-browser")) {
    const std::string path = std::string(pref) + "pp-browser-debug.log";
    SDL_free(pref);
    g_log_file = std::fopen(path.c_str(), "a");
    if (g_log_file) {
      std::fprintf(g_log_file, "---- pp-browser log open ----\n");
      std::fflush(g_log_file);
    }
  }
}

void WriteIosLog(logging::Level level, const std::string& message) {
  EnsureIosLogFile();
  const char* tag = "I";
  os_log_type_t os_type = OS_LOG_TYPE_INFO;
  switch (level) {
  case logging::kLevelDebug:
    tag = "D";
    os_type = OS_LOG_TYPE_DEBUG;
    break;
  case logging::Level::INFO:
    tag = "I";
    os_type = OS_LOG_TYPE_INFO;
    break;
  case logging::Level::WARNING:
    tag = "W";
    os_type = OS_LOG_TYPE_DEFAULT;
    break;
  case logging::kLevelError:
  case logging::Level::CRITICAL:
    tag = "E";
    os_type = OS_LOG_TYPE_ERROR;
    break;
  }
  os_log_with_type(OS_LOG_DEFAULT, os_type, "[%{public}s] %{public}s", kProductLogTag, message.c_str());
  std::fprintf(stderr, "[%s][%s] %s\n", kProductLogTag, tag, message.c_str());
  std::lock_guard<std::mutex> lock(g_file_mu);
  if (g_log_file) {
    std::fprintf(g_log_file, "[%s] %s\n", tag, message.c_str());
    std::fflush(g_log_file);
  }
}
#endif

class PlatformLogHandler : public logging::Handler {
public:
  void emit(logging::Level level, const std::string& /*loggerName*/, const std::string& message) override {
    if (level < level_) {
      return;
    }
#if defined(__ANDROID__)
    int priority = ANDROID_LOG_INFO;
    switch (level) {
    case logging::kLevelDebug:
      priority = ANDROID_LOG_DEBUG;
      break;
    case logging::Level::INFO:
      priority = ANDROID_LOG_INFO;
      break;
    case logging::Level::WARNING:
      priority = ANDROID_LOG_WARN;
      break;
    case logging::kLevelError:
      priority = ANDROID_LOG_ERROR;
      break;
    case logging::Level::CRITICAL:
      priority = ANDROID_LOG_FATAL;
      break;
    }
    __android_log_write(priority, kProductLogTag, message.c_str());
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    WriteIosLog(level, message);
#else
    std::cout << message << std::endl;
#endif
  }
};

} // namespace

void InstallPlatformLogSink() {
  // Desktop: root ConsoleHandler (stderr) is installed in Logger.cpp.
  // Mobile: add a platform handler (logcat / os_log + file).
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  logging::getRootLogger().addHandler(std::make_shared<PlatformLogHandler>());
#endif
}

} // namespace pbr
