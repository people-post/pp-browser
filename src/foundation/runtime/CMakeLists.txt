# Headless-safe runtime: worker pool + coordinator + main/UI mailbox (pp-node, tests).
pp_browser_add_foundation_library(pp_foundation_runtime_core
  SOURCES
    AppRuntime.cpp
    CoordinatorThread.cpp
    ThreadRuntime.cpp
    WorkerDispatch.cpp
)

target_compile_definitions(pp_foundation_runtime_core PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

if(PP_BROWSER_HEADLESS)
  return()
endif()

# GUI process runtime: lifecycle + background sync cadence (mailbox is in runtime_core).
pp_browser_add_foundation_library(pp_foundation_runtime
  SOURCES
    AppLifecycle.cpp
    BackgroundSyncScheduler.cpp
  PUBLIC_LIBS
    pp_foundation_runtime_core)


if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
