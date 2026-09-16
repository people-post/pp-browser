pp_browser_add_foundation_library(pp_foundation_diagnostics
  SOURCES
    CrashBreadcrumbs.cpp
    CrashDump.cpp
  PUBLIC_LIBS
    pp_foundation_runtime_core)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
