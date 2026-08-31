# iOS .app bundle metadata and asset staging (pp-browser repo, PP product name).

set(PP_BROWSER_IOS_BUNDLE_ID "dev.pp-browser.ios" CACHE STRING "iOS CFBundleIdentifier")
set(PP_BROWSER_IOS_BUNDLE_NAME "${PP_BROWSER_PRODUCT_NAME}" CACHE STRING
  "iOS CFBundleDisplayName / bundle folder (kProductBundleName)")
# Prefer the value set before project() in the top-level CMakeLists (required for Ninja).
if(NOT PP_BROWSER_IOS_DEPLOYMENT_TARGET)
  set(PP_BROWSER_IOS_DEPLOYMENT_TARGET "15.0" CACHE STRING
    "Minimum iOS version (IPHONEOS_DEPLOYMENT_TARGET / CMAKE_OSX_DEPLOYMENT_TARGET)")
endif()

function(pp_browser_configure_ios_app target)
  if(NOT PP_BROWSER_IS_IOS)
    return()
  endif()

  # Reinforce for Ninja incremental builds / late includes (no-op if already set).
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "${PP_BROWSER_IOS_DEPLOYMENT_TARGET}")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${PP_BROWSER_IOS_DEPLOYMENT_TARGET}" CACHE STRING
      "Minimum iOS version for linked binaries" FORCE)
  endif()

  set(_ios_info_plist "${CMAKE_SOURCE_DIR}/packaging/ios/Info.plist")
  if(NOT EXISTS "${_ios_info_plist}")
    message(FATAL_ERROR "Missing iOS Info.plist: ${_ios_info_plist}")
  endif()

  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "${PP_BROWSER_IOS_BUNDLE_NAME}"
    MACOSX_BUNDLE_INFO_PLIST "${_ios_info_plist}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${PP_BROWSER_IOS_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "${PP_BROWSER_IOS_BUNDLE_NAME}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PP_BROWSER_RELEASE_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PP_BROWSER_RELEASE_VERSION}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${PP_BROWSER_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "YOUR_TEAM_ID"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_PROVISIONING_PROFILE_SPECIFIER "pp-browser iOS Development"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${PP_BROWSER_IOS_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
  )

  set_source_files_properties("${CMAKE_SOURCE_DIR}/assets/branding/app-icon.png" PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
  target_sources(${target} PRIVATE "${CMAKE_SOURCE_DIR}/assets/branding/app-icon.png")

  set(_ios_assets_dir "${CMAKE_BINARY_DIR}/ios-assets")
  add_custom_target(pp_browser_ios_assets ALL
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_ios_assets_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ios_assets_dir}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${CMAKE_SOURCE_DIR}/assets" "${_ios_assets_dir}/assets"
    COMMENT "Staging iOS bundle assets"
    VERBATIM
  )
  add_dependencies(${target} pp_browser_ios_assets)

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${_ios_assets_dir}/assets"
      "$<TARGET_BUNDLE_DIR:${target}>/assets"
    COMMENT "Copying assets into iOS app bundle"
    VERBATIM
  )

  install(TARGETS ${target} BUNDLE DESTINATION .)
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/"
    DESTINATION "${PP_BROWSER_IOS_BUNDLE_NAME}.app/assets"
    USE_SOURCE_PERMISSIONS)
endfunction()
