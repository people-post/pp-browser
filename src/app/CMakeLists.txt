set(PP_BROWSER_APP_SOURCES
  main.cpp
  Application.cpp
  Bootstrap.cpp
  ConfigApplyBridge.cpp
)

if(ANDROID)
  add_library(${PP_BROWSER_APP_TARGET} SHARED ${PP_BROWSER_APP_SOURCES})
  set_target_properties(${PP_BROWSER_APP_TARGET} PROPERTIES OUTPUT_NAME main)
else()
  add_executable(${PP_BROWSER_APP_TARGET} ${PP_BROWSER_APP_SOURCES})
endif()

option(PP_BROWSER_PACKAGED_BUILD "Build for installable/packaged release (runtime asset paths)" OFF)

set(_pp_browser_compile_defs RMLUI_STATIC_LIB)
if(PP_BROWSER_PACKAGED_BUILD OR PP_BROWSER_IS_MOBILE)
  list(APPEND _pp_browser_compile_defs PP_BROWSER_PACKAGED_BUILD=1)
elseif(NOT PP_BROWSER_IS_MOBILE)
  list(APPEND _pp_browser_compile_defs PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
endif()

target_compile_definitions(${PP_BROWSER_APP_TARGET} PRIVATE ${_pp_browser_compile_defs})

if(WIN32)
  set_target_properties(${PP_BROWSER_APP_TARGET} PROPERTIES WIN32_EXECUTABLE TRUE)
  target_sources(${PP_BROWSER_APP_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/assets/branding/app-icon.rc)
endif()

if(PP_BROWSER_IS_IOS)
  include(IosBundle)
  pp_browser_configure_ios_app(${PP_BROWSER_APP_TARGET})
elseif(APPLE AND PP_BROWSER_PACKAGED_BUILD)
  set(_macos_info_plist "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist")
  if(NOT EXISTS "${_macos_info_plist}")
    message(FATAL_ERROR "Missing macOS Info.plist: ${_macos_info_plist}")
  endif()
  set_target_properties(${PP_BROWSER_APP_TARGET} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${_macos_info_plist}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.pp-browser.app"
    MACOSX_BUNDLE_BUNDLE_NAME "Frame"
    MACOSX_BUNDLE_ICON_FILE app-icon.png
    MACOSX_BUNDLE_BUNDLE_VERSION "${PP_BROWSER_RELEASE_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PP_BROWSER_RELEASE_VERSION}"
  )
  set_source_files_properties(${CMAKE_SOURCE_DIR}/assets/branding/app-icon.png PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
  target_sources(${PP_BROWSER_APP_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/assets/branding/app-icon.png)
endif()

target_include_directories(${PP_BROWSER_APP_TARGET} PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/libp2p/fork/include
  ${CMAKE_SOURCE_DIR}/src/render/fork/Source/Core/FontEngineHarfBuzz
)

target_link_libraries(${PP_BROWSER_APP_TARGET} PRIVATE
  pp_feature
  pp_base
  pp_common
  pp_rmlui_backend
  RmlUi::Core
  p2p
  nlohmann_json::nlohmann_json
  CURL::libcurl
  Threads::Threads
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(${PP_BROWSER_APP_TARGET} PRIVATE OpenSSL::Crypto)
endif()

option(PPBROWSER_ENABLE_DEBUGGER "Link RmlUi debugger" ${PPBROWSER_ENABLE_DEBUGGER_DEFAULT})
option(PPBROWSER_LOCAL_LLM "Enable local LLM backend (not implemented)" OFF)
if(PPBROWSER_LOCAL_LLM)
  message(STATUS "PPBROWSER_LOCAL_LLM requested; implementation deferred")
endif()
if(PPBROWSER_ENABLE_DEBUGGER)
  target_link_libraries(${PP_BROWSER_APP_TARGET} PRIVATE RmlUi::Debugger)
  target_compile_definitions(${PP_BROWSER_APP_TARGET} PRIVATE PPBROWSER_ENABLE_DEBUGGER=1)
endif()

if(NOT PP_BROWSER_IS_MOBILE)
  pp_configure_status("Configure step complete")

  if(APPLE AND PP_BROWSER_PACKAGED_BUILD)
    install(TARGETS ${PP_BROWSER_APP_TARGET} BUNDLE DESTINATION .)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/ DESTINATION Frame.app/Contents/Resources/assets)
  elseif(WIN32 AND PP_BROWSER_PACKAGED_BUILD)
    install(TARGETS ${PP_BROWSER_APP_TARGET} RUNTIME DESTINATION .)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/ DESTINATION assets)
  else()
    install(TARGETS ${PP_BROWSER_APP_TARGET} RUNTIME DESTINATION bin)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/ DESTINATION share/pp-browser/assets)
  endif()

  if(PP_BROWSER_PACKAGED_BUILD AND (APPLE OR WIN32))
    include(CPackOptions)
  endif()
elseif(PP_BROWSER_IS_IOS)
  pp_configure_status("iOS target: install with cmake --install (see docs/ops/IOS_BUILD.md)")
else()
  pp_configure_status("Mobile target: skipping host tests and desktop install rules")
endif()

# Alias for scripts/docs that refer to pp-browser on all platforms.
if(ANDROID AND NOT TARGET pp-browser)
  add_library(pp-browser ALIAS main)
endif()

if(NOT PP_BROWSER_IS_MOBILE)
  add_subdirectory(node)
endif()
