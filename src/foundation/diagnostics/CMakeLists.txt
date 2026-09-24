pp_browser_add_foundation_library(pp_foundation_diagnostics
  SOURCES
    CrashBreadcrumbs.cpp
    CrashDump.cpp
    CrashReportEnvelope.cpp
    LogFile.cpp
  PUBLIC_LIBS
    pp_foundation_runtime_core)
# dladdr (CrashDump image_base) — libdl on older glibc.
target_link_libraries(pp_foundation_diagnostics PRIVATE ${CMAKE_DL_LIBS})

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
