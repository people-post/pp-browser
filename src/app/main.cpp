#include "app/Application.h"
#include "app/Bootstrap.h"
#include "common/Logger.h"
#include "common/StartupTiming.h"
#include "base/platform/Platform.h"
#include "base/platform/PlatformLogDefaults.h"
#include "base/platform/PlatformLogSink.h"
#include "base/platform/PlatformStartupHints.h"
#include "base/runtime/ProductBranding.h"

#include <SDL3/SDL_main.h>

#include <cstdlib>
#include <cstring>

namespace {

bool EnvTruthy(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
         std::strcmp(value, "FALSE") != 0;
}

const char* LevelName(pbr::logging::Level level) {
  switch (level) {
  case pbr::logging::kLevelDebug:
    return "DEBUG";
  case pbr::logging::Level::INFO:
    return "INFO";
  case pbr::logging::Level::WARNING:
    return "WARNING";
  case pbr::logging::kLevelError:
    return "ERROR";
  case pbr::logging::Level::CRITICAL:
    return "CRITICAL";
  }
  return "UNKNOWN";
}

} // namespace

int main(int argc, char** argv) {
  bool debug_mode = false;
  bool startup_timing = EnvTruthy("PP_BROWSER_STARTUP_TIMING");
  std::string profile_override;
  std::string pin;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--debug") == 0) {
      debug_mode = true;
    } else if (std::strcmp(argv[i], "--startup-timing") == 0) {
      startup_timing = true;
    } else if (std::strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile_override = argv[i + 1];
      ++i;
    } else if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
      pin = argv[i + 1];
      ++i;
    }
  }

  auto root = pbr::logging::getRootLogger();
  const auto default_level = pbr::DefaultRootLogLevel(debug_mode);
  root.setLevel(default_level);
  pbr::logging::setEmitFloor(pbr::DefaultEmitFloor(debug_mode));
  // Install early so [startup] marks reach logcat before Bootstrap.
  pbr::InstallPlatformLogSink();
  if (startup_timing) {
    pbr::EnableStartupTimingLogs();
  }
  (void)pbr::StartupEpoch();
  pbr::StartupMark("main_enter");
  root.info << "Logging level set to " << LevelName(root.getLevel())
            << " emit_floor=" << LevelName(pbr::logging::getEmitFloor())
            << (startup_timing ? " (startup timing enabled)" : "");

  if (!pbr::Platform::EarlyInit()) {
    root.error << "Platform early init failed";
    return 1;
  }
  pbr::StartupMark("after_early_init");

  if (!profile_override.empty()) {
    root.warning << "Using profile override '" << profile_override
                 << "' (multi-profile UI is not shipped yet)";
  }

  pbr::BootstrapOptions options;
  options.argc = argc;
  options.argv = argv;
  options.profile_override = profile_override;
  options.pin = pin;

  // Construct Application first so MessagingHub is process-owned before Bootstrap initializes it.
  pbr::Application app;

  auto bootstrap_result = [&] {
    pbr::StartupPhase phase("Bootstrap::Run");
    return pbr::Bootstrap::Run(options, app.Messaging());
  }();
  if (!bootstrap_result) {
    root.error << pbr::kProductName << ": " << bootstrap_result.error().message;
    return 1;
  }

  app.Store().Initialize(std::move(bootstrap_result.value()));
  pbr::StartupMark("after_session_store");

  if (![&] {
        pbr::StartupPhase phase("Application::Initialize");
        return app.Initialize(pbr::kProductName);
      }()) {
    root.error << pbr::kProductName << ": failed to initialize." << pbr::InitFailureHint();
    return 1;
  }
  pbr::StartupMark("enter_Application::Run");
  app.Run();
  app.Shutdown();
  return 0;
}
