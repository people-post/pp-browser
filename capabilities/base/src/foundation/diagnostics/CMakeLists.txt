pp_browser_add_foundation_library(pp_foundation_diagnostics
  SOURCES
    CrashBreadcrumbs.cpp
    CrashDump.cpp
    CrashReportEnvelope.cpp
    DiagnosticsBundle.cpp
    DiagnosticsSelfCheck.cpp
    DiagnosticsUploadResult.cpp
    LogFile.cpp
  PUBLIC_LIBS
    pp_foundation_runtime_core)
# Peak resident memory for metrics (OsPeakRss): getrusage on POSIX, unknown elsewhere.
if(PP_BROWSER_DESKTOP_OS STREQUAL "WIN32")
  target_sources(pp_foundation_diagnostics PRIVATE OsPeakRss_Default.cpp)
else()
  target_sources(pp_foundation_diagnostics PRIVATE OsPeakRss_Posix.cpp)
endif()
# Current memory for metrics (OsCurrentMemory): task_info on Apple, /proc on Linux / Android, GetProcessMemoryInfo on
# Windows.
if(APPLE)
  target_sources(pp_foundation_diagnostics PRIVATE OsCurrentMemory_Darwin.cpp)
elseif(PP_BROWSER_DESKTOP_OS STREQUAL "WIN32")
  target_sources(pp_foundation_diagnostics PRIVATE OsCurrentMemory_Win32.cpp)
  target_link_libraries(pp_foundation_diagnostics PRIVATE psapi)
else()
  target_sources(pp_foundation_diagnostics PRIVATE OsCurrentMemory_Posix.cpp)
endif()
# dladdr (CrashDump image_base) — libdl on older glibc.
target_link_libraries(pp_foundation_diagnostics PRIVATE ${CMAKE_DL_LIBS})

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
