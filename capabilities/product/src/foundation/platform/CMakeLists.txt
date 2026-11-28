include(PpBrowserBase)

# The product's platform core (capability-repos CR028): the backends a build can run and what is compiled in for them
# (origins, relay ids, relay certificates, the tester gate, relay.content_sha256), the platform, the paths the apps keep
# their files under, the log defaults, and the OS helpers pp-node uses (the executable's path, the data-folder lock,
# process stats, thread names). Headless; both apps. src/foundation/platform adds the app's on top.
set(_pp_product_platform_sources
  DeploymentProfile.cpp
  BackendTable.cpp
  Platform.cpp
  PlatformLogDefaults.cpp
  IPathProvider.cpp
  DesktopPathProvider.cpp
  desktop/PathHelpers.cpp
  ExecutablePath.cpp
  os/OsThreadName_Posix.cpp
  os/OsDirLock_Posix.cpp
)
if(PP_BROWSER_DESKTOP_OS STREQUAL "WIN32")
  list(APPEND _pp_product_platform_sources
    os/OsThreadName_Win32.cpp
    os/OsDirLock_Win32.cpp
    os/OsExecutablePath_Win32.cpp
    os/OsProcessStats_Default.cpp
    desktop/PathProvider_Win32.cpp)
elseif(PP_BROWSER_DESKTOP_OS STREQUAL "DARWIN")
  list(APPEND _pp_product_platform_sources
    os/OsExecutablePath_Darwin.cpp
    os/OsProcessStats_Default.cpp
    desktop/PathProvider_Darwin.cpp)
else()
  list(APPEND _pp_product_platform_sources
    os/OsExecutablePath_Linux.cpp
    os/OsProcessStats_Linux.cpp
    desktop/PathProvider_Linux.cpp)
endif()

pp_browser_add_foundation_library(pp_product_platform
  SOURCES
    ${_pp_product_platform_sources}
  PUBLIC_LIBS
    pp_foundation_product
)

# DesktopPathProvider::BundleAssetsDir() reads these; they must be set on the
# target that compiles it, not only on the app target (which links this lib).
# Without them the packaged branch is compiled out and assets resolve relative
# to the working directory, so a launched PP.app finds no assets.
if(PP_BROWSER_PACKAGED_BUILD OR PP_BROWSER_IS_MOBILE)
  target_compile_definitions(pp_product_platform PRIVATE PP_BROWSER_PACKAGED_BUILD=1)
else()
  target_compile_definitions(pp_product_platform PRIVATE
    PP_BROWSER_ASSETS_DIR="${PP_BROWSER_ROOT}/assets")
endif()

# Compile-time backend origins (scheme://host, no path); PP_BROWSER_ORIGIN overrides them at runtime.
# Empty by default: backend-dependent features stay disabled until a prod origin is supplied;
# an empty staging / dev origin hides that backend option (a store build ships neither, RC015).
set(PP_API_URL_PROD "" CACHE STRING "Prod backend origin compiled into the client")
set(PP_API_URL_STAGING "" CACHE STRING "Staging backend origin compiled into the client")
set(PP_API_URL_DEV "" CACHE STRING "Dev backend origin compiled into the client")
foreach(origin_var PP_API_URL_PROD PP_API_URL_STAGING PP_API_URL_DEV)
  if(NOT "${${origin_var}}" STREQUAL "")
    target_compile_definitions(pp_product_platform PRIVATE
      ${origin_var}="${${origin_var}}")
  endif()
endforeach()
# The org relays' ids beside their origins (release-compatibility SIGNED_FORMATS § 3.3, § 3.5: a relay address is
# its origin and its relay id, `relay:` + 43 base64url characters; the app never learns it from the relay's own
# answer). `pending` (the default): that environment's operator set is not made yet — it fails closed, the relay is
# not used, and release checks refuse it (scripts/check/check_relay_ids.sh). Empty: the relay is pinned on its first
# valid certificate, with a warning (dogfood).
set(PP_RELAY_ID_PROD "pending" CACHE STRING "Relay id of the prod backend's relay (beside PP_API_URL_PROD)")
set(PP_RELAY_ID_STAGING "pending" CACHE STRING "Relay id of the staging backend's relay (beside PP_API_URL_STAGING)")
set(PP_RELAY_ID_DEV "pending" CACHE STRING "Relay id of the dev backend's relay (beside PP_API_URL_DEV)")
foreach(relay_id_var PP_RELAY_ID_PROD PP_RELAY_ID_STAGING PP_RELAY_ID_DEV)
  set(_relay_id "${${relay_id_var}}")
  if(NOT _relay_id STREQUAL "" AND NOT _relay_id STREQUAL "pending" AND NOT _relay_id MATCHES "^relay:[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "${relay_id_var} is not a relay id (relay:…, from the relay's link or GET /v1/relay-identity), "
                        "\"pending\" or empty")
  endif()
  string(LENGTH "${_relay_id}" _relay_id_length)
  if(_relay_id MATCHES "^relay:" AND NOT _relay_id_length EQUAL 49)
    message(FATAL_ERROR "${relay_id_var} is not a relay id: relay: and 43 base64url characters")
  endif()
endforeach()
target_compile_definitions(pp_product_platform PRIVATE
  PP_RELAY_ID_PROD="${PP_RELAY_ID_PROD}"
  PP_RELAY_ID_STAGING="${PP_RELAY_ID_STAGING}"
  PP_RELAY_ID_DEV="${PP_RELAY_ID_DEV}")
# A tester build (release-compatibility RC022): an internal/<n> TestFlight upload, a tester APK, a desktop tester
# package. It shares the store app's data on a phone, so it never offers or opens production: CMake refuses a prod
# origin and a prod relay id in it, compiles no prod certificate, and the app never runs prod or opens a `prod` /
# `custom-…` profile (DeploymentProfile.h BuildKind). OFF: a store build (release, release candidate) or a developer's.
option(PP_TESTER_BUILD "Tester build: no production backend, tester profiles only (release-compatibility RC022)" OFF)
if(PP_TESTER_BUILD)
  if(PP_BROWSER_UI_DEMO)
    message(FATAL_ERROR "PP_TESTER_BUILD is ON: the UI demo app is a developer build of its own, not a tester build")
  endif()
  if(NOT "${PP_API_URL_PROD}" STREQUAL "")
    message(FATAL_ERROR "PP_TESTER_BUILD is ON: a tester build compiles no prod origin (PP_API_URL_PROD). Configure it "
                        "with -DPP_API_URL_PROD= and staging / dev origins only (release-compatibility RC022).")
  endif()
  if(NOT "${PP_RELAY_ID_PROD}" STREQUAL "" AND NOT "${PP_RELAY_ID_PROD}" STREQUAL "pending")
    message(FATAL_ERROR "PP_TESTER_BUILD is ON: a tester build compiles no prod relay id (PP_RELAY_ID_PROD). Leave it "
                        "pending (-DPP_RELAY_ID_PROD=pending; release-compatibility RC022).")
  endif()
  target_compile_definitions(pp_product_platform PRIVATE PP_TESTER_BUILD=1)
endif()

# Each environment's newest relay certificate at build time (SIGNED_FORMATS § 3.5, Q10): the floor a fresh install
# starts from, so it knows the audience before its first fetch and never goes below it. A file holds the certificate's
# standard base64 (`relay-operator.mjs combine` prints it; lines may wrap), `#` comment lines, or `pending` (none yet:
# the relay's own certificate is fetched). Generated into CompiledRelayCertificates.inc as string literals.
set(PP_RELAY_CERT_FILE_PROD "${PP_BROWSER_ROOT}/config/relay-certs/prod.cert" CACHE FILEPATH
    "The prod relay's certificate compiled into the client")
set(PP_RELAY_CERT_FILE_STAGING "${PP_BROWSER_ROOT}/config/relay-certs/staging.cert" CACHE FILEPATH
    "The staging relay's certificate compiled into the client")
set(PP_RELAY_CERT_FILE_DEV "${PP_BROWSER_ROOT}/config/relay-certs/dev.cert" CACHE FILEPATH
    "The dev relay's certificate compiled into the client")
function(pp_browser_relay_certificate_literal file out_var)
  set(_text "")
  if(NOT file STREQUAL "" AND EXISTS "${file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    file(READ "${file}" _text)
    # Comment lines out, then every space and line break (a wrapped certificate is one base64 text).
    string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" _text "${_text}")
    string(REGEX REPLACE "[ \t\r\n]" "" _text "${_text}")
  endif()
  if(_text STREQUAL "pending")
    set(_text "")
  endif()
  if(NOT _text STREQUAL "" AND NOT _text MATCHES "^[A-Za-z0-9+/]+=*$")
    message(FATAL_ERROR "${file} is not a relay certificate (standard base64), `pending` or empty")
  endif()
  # Pieces of 2000 characters: one string literal stays within every compiler's limit.
  set(_literal "\"\"")
  string(LENGTH "${_text}" _length)
  set(_at 0)
  while(_at LESS _length)
    string(SUBSTRING "${_text}" ${_at} 2000 _piece)
    string(APPEND _literal "\n    \"${_piece}\"")
    math(EXPR _at "${_at} + 2000")
  endwhile()
  set(${out_var} "${_literal}" PARENT_SCOPE)
endfunction()
if(PP_TESTER_BUILD)
  set(_relay_cert_prod "\"\"") # a tester build compiles no prod certificate (RC022)
else()
  pp_browser_relay_certificate_literal("${PP_RELAY_CERT_FILE_PROD}" _relay_cert_prod)
endif()
pp_browser_relay_certificate_literal("${PP_RELAY_CERT_FILE_STAGING}" _relay_cert_staging)
pp_browser_relay_certificate_literal("${PP_RELAY_CERT_FILE_DEV}" _relay_cert_dev)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/CompiledRelayCertificates.inc" CONTENT
"// Generated by capabilities/product/src/foundation/platform/CMakeLists.txt from PP_RELAY_CERT_FILE_PROD / _STAGING / _DEV.
namespace pbr {
namespace {
constexpr const char kCompiledRelayCertificateProd[] = ${_relay_cert_prod};
constexpr const char kCompiledRelayCertificateStaging[] = ${_relay_cert_staging};
constexpr const char kCompiledRelayCertificateDev[] = ${_relay_cert_dev};
} // namespace
} // namespace pbr
")
target_include_directories(pp_product_platform PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Compile-time default of `relay.content_sha256`: send x-amz-content-sha256 on relay POST/PUT. Off unless the
# deployment's relay API sits behind CloudFront origin access control; config.json / PP_NODE_RELAY_CONTENT_SHA256 override.
option(PP_RELAY_CONTENT_SHA256 "Default relay.content_sha256 to true in this build" OFF)
if(PP_RELAY_CONTENT_SHA256)
  target_compile_definitions(pp_product_platform PRIVATE PP_RELAY_CONTENT_SHA256=1)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
