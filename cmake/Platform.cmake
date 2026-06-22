# Platform detection and mobile defaults for pp-browser.

set(PP_BROWSER_IS_ANDROID FALSE)
set(PP_BROWSER_IS_IOS FALSE)
set(PP_BROWSER_IS_MOBILE FALSE)

if(ANDROID)
  set(PP_BROWSER_IS_ANDROID TRUE)
  set(PP_BROWSER_IS_MOBILE TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(PP_BROWSER_IS_IOS TRUE)
  set(PP_BROWSER_IS_MOBILE TRUE)
endif()

option(PP_BROWSER_MOBILE "Target is a mobile platform (Android or iOS)" "${PP_BROWSER_IS_MOBILE}")

if(PP_BROWSER_IS_MOBILE AND NOT PP_BROWSER_MOBILE)
  set(PP_BROWSER_MOBILE ON CACHE BOOL "Target is a mobile platform (Android or iOS)" FORCE)
endif()

if(PP_BROWSER_IS_MOBILE)
  set(PPBROWSER_ENABLE_DEBUGGER_DEFAULT OFF)
else()
  set(PPBROWSER_ENABLE_DEBUGGER_DEFAULT ON)
endif()

if(PP_BROWSER_IS_ANDROID)
  set(PP_BROWSER_APP_TARGET main)
else()
  set(PP_BROWSER_APP_TARGET pp-browser)
endif()

message(STATUS "pp-browser platform: android=${PP_BROWSER_IS_ANDROID} ios=${PP_BROWSER_IS_IOS} mobile=${PP_BROWSER_MOBILE} app_target=${PP_BROWSER_APP_TARGET}")
