# Headless-safe runtime: worker pool + coordinator (pp-node, tests).
pp_browser_add_base_library(pp_base_runtime_core
  SOURCES
    AppRuntime.cpp
    CoordinatorThread.cpp
    ThreadRuntime.cpp
)

target_compile_definitions(pp_base_runtime_core PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

if(PP_BROWSER_HEADLESS)
  return()
endif()

# GUI process runtime: UI mailbox, lifecycle, background sync cadence.
pp_browser_add_base_library(pp_base_runtime
  SOURCES
    AppLifecycle.cpp
    AppRuntimeGui.cpp
    BackgroundSyncScheduler.cpp
  PUBLIC_LIBS
    pp_base_runtime_core)


if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
