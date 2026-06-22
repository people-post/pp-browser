#include "platform/PlatformLogSink.h"

#include "log/Logger.h"

#include <iostream>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace pbr {

namespace {

class PlatformLogHandler : public logging::Handler {
public:
  void emit(logging::Level level, const std::string& /*loggerName*/, const std::string& message) override {
    if (level < level_) {
      return;
    }
#if defined(__ANDROID__)
    int priority = ANDROID_LOG_INFO;
    switch (level) {
    case logging::Level::DEBUG:
      priority = ANDROID_LOG_DEBUG;
      break;
    case logging::Level::INFO:
      priority = ANDROID_LOG_INFO;
      break;
    case logging::Level::WARNING:
      priority = ANDROID_LOG_WARN;
      break;
    case logging::Level::ERROR:
      priority = ANDROID_LOG_ERROR;
      break;
    case logging::Level::CRITICAL:
      priority = ANDROID_LOG_FATAL;
      break;
    }
    __android_log_write(priority, "pp-browser", message.c_str());
#else
    std::cout << message << std::endl;
#endif
  }
};

} // namespace

void InstallPlatformLogSink() {
#if defined(__ANDROID__)
  logging::getRootLogger().addHandler(std::make_shared<PlatformLogHandler>());
#endif
}

} // namespace pbr
