pp_browser_add_foundation_library(pp_foundation_error
  SOURCES AppError.cpp CurlNetworkError.cpp
  PUBLIC_LIBS pp_pbr_common
  PRIVATE_LIBS CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
