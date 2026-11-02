# Headless-safe runtime: pp-cpp-common's AppRuntime (worker pool, coordinator, UI mailbox, owner
# threads; common/runtime/) with the product's owners registered, the executors every mesh takes
# (AppRuntimeMeshExecutors), and the product version (pp-node, tests).
pp_browser_add_foundation_library(pp_foundation_runtime_core
  SOURCES
    AppRuntimeMeshExecutors.cpp
  PUBLIC_LIBS
    pp_common
    pp_mesh_types
)

# Time to finish a transfer in the background (BackgroundActivity): iOS asks UIKit; elsewhere nothing to ask.
# Here (base) rather than foundation/platform: capabilities hold one and may not include the product's platform.
if(PP_BROWSER_IS_IOS)
  target_sources(pp_foundation_runtime_core PRIVATE BackgroundActivity_Ios.mm)
  target_link_libraries(pp_foundation_runtime_core PUBLIC "-framework UIKit")
else()
  target_sources(pp_foundation_runtime_core PRIVATE BackgroundActivity_Default.cpp)
endif()

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

