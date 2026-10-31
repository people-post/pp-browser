# Headless-safe runtime: pp-cpp-common's AppRuntime (worker pool, coordinator, UI mailbox, owner
# threads; common/runtime/) with the product's owners registered, and the product version (pp-node, tests).
pp_browser_add_foundation_library(pp_foundation_runtime_core
  SOURCES
    ProductOwners.cpp
  PUBLIC_LIBS
    pp_common
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

