#include "foundation/platform/PlatformLogSink.h"

#include "foundation/runtime/ProductBranding.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <cstdio>
#include <cstdlib>
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

std::mutex g_file_mu;
FILE* g_env_log_file = nullptr;

const char* LevelTag(logging::Level level) {
  switch (level) {
  case logging::kLevelDebug:
    return "D";
  case logging::Level::INFO:
    return "I";
  case logging::Level::WARNING:
    return "W";
  case logging::kLevelError:
  case logging::Level::CRITICAL:
    return "E";
  }
  return "I";
}

/** Optional desktop/file sink when PP_BROWSER_LOG_FILE is set (run-pp-browser.bat). */
class EnvFileLogHandler : public logging::Handler {
public:
  void emit(logging::Level level, const std::string& /*loggerName*/, const std::string& message) override {
    if (level < level_ || !g_env_log_file) {
      return;
    }
    std::lock_guard<std::mutex> lock(g_file_mu);
    if (!g_env_log_file) {
      return;
    }
    std::fprintf(g_env_log_file, "[%s] %s\n", LevelTag(level), message.c_str());
    std::fflush(g_env_log_file);
  }
};

bool TryOpenEnvLogFile() {
  const char* path = std::getenv("PP_BROWSER_LOG_FILE");
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_file_mu);
  if (g_env_log_file) {
    return true;
  }
  g_env_log_file = std::fopen(path, "a");
  if (!g_env_log_file) {
    return false;
  }
  std::fprintf(g_env_log_file, "---- pp-browser log open path=%s ----\n", path);
  std::fflush(g_env_log_file);
  return true;
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
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
  const char* tag = LevelTag(level);
  os_log_type_t os_type = OS_LOG_TYPE_INFO;
  switch (level) {
  case logging::kLevelDebug:
    os_type = OS_LOG_TYPE_DEBUG;
    break;
  case logging::Level::INFO:
    os_type = OS_LOG_TYPE_INFO;
    break;
  case logging::Level::WARNING:
    os_type = OS_LOG_TYPE_DEFAULT;
    break;
  case logging::kLevelError:
  case logging::Level::CRITICAL:
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
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;

  // Desktop dogfood: PP_BROWSER_LOG_FILE (set by run-pp-browser.bat) — GUI apps often
  // have no usable stdout/stderr, so open the file sink ourselves.
  if (TryOpenEnvLogFile()) {
    logging::getRootLogger().addHandler(std::make_shared<EnvFileLogHandler>());
    if (const char* path = std::getenv("PP_BROWSER_LOG_FILE"); path && path[0]) {
      logging::getRootLogger().info << "Desktop process log file=" << path;
    }
  }

  // Mobile: add a platform handler (logcat / os_log + file).
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
  logging::getRootLogger().addHandler(std::make_shared<PlatformLogHandler>());
#endif
}

} // namespace pbr
