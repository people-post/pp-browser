# Provides android_log::android_log for vendored protobuf on Android NDK builds.
include(FindPackageHandleStandardArgs)

if(NOT ANDROID)
  set(android_log_FOUND FALSE)
  return()
endif()

find_library(ANDROID_LOG_LIBRARY NAMES log)

find_package_handle_standard_args(android_log DEFAULT_MSG ANDROID_LOG_LIBRARY)

if(android_log_FOUND AND NOT TARGET android_log::android_log)
  add_library(android_log::android_log UNKNOWN IMPORTED)
  set_target_properties(android_log::android_log PROPERTIES
    IMPORTED_LOCATION "${ANDROID_LOG_LIBRARY}")
endif()

mark_as_advanced(ANDROID_LOG_LIBRARY)
