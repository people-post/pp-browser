# UI demo app (src/foundation/product/DevTools.h kUiDemoBuild): a separate developer-only app that seeds fixed fake
# data and has no backend address, used to compare screens with the design on the iOS simulator / Android.
# Never set for release, TestFlight or store builds. Plan: DEV/PLAN-ui-demo-data.md.
option(PP_BROWSER_UI_DEMO "Separate UI demo app with fixed fake data and no backend (iOS simulator / Android only)" OFF)

if(PP_BROWSER_UI_DEMO)
  if(NOT PP_BROWSER_DEV_TOOLS)
    message(FATAL_ERROR "PP_BROWSER_UI_DEMO needs -DPP_BROWSER_DEV_TOOLS=ON (developer builds only)")
  endif()
  if(NOT "${PP_API_URL_PROD}" STREQUAL "" OR NOT "${PP_API_URL_QA}" STREQUAL "")
    message(FATAL_ERROR "PP_BROWSER_UI_DEMO must be built without PP_API_URL_PROD / PP_API_URL_QA (the demo app has no backend)")
  endif()
  if(PP_BROWSER_IS_IOS)
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" _pp_ios_sysroot)
    if(NOT _pp_ios_sysroot MATCHES "iphonesimulator")
      message(FATAL_ERROR "PP_BROWSER_UI_DEMO on iOS is for the simulator only (CMAKE_OSX_SYSROOT='${CMAKE_OSX_SYSROOT}')")
    endif()
  elseif(NOT PP_BROWSER_IS_ANDROID)
    message(FATAL_ERROR "PP_BROWSER_UI_DEMO is for the iOS simulator and Android only: a desktop demo build would share the data directory and config.json with the real desktop app")
  endif()
  add_compile_definitions(PP_BROWSER_UI_DEMO=1)
  message(STATUS "pp-browser: PP_BROWSER_UI_DEMO=ON (separate demo app, no backend)")
endif()
