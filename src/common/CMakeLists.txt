set(PP_BROWSER_COMMON_SOURCES
  Module.cpp
  Logger.cpp
  CivilTime.cpp
  Utilities.cpp
  SequencedTaskRunner.cpp
  WorkerPool.cpp
  WorkerDispatch.cpp
)

include(PpBrowserWarnings)

add_library(pp_common STATIC ${PP_BROWSER_COMMON_SOURCES})
target_include_directories(pp_common PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
pp_browser_apply_warnings(pp_common)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
