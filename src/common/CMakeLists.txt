set(PP_BROWSER_COMMON_SOURCES
  Module.cpp
  Logger.cpp
  Utilities.cpp
  SequencedTaskRunner.cpp
)

add_library(pp_common STATIC ${PP_BROWSER_COMMON_SOURCES})
target_include_directories(pp_common PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
