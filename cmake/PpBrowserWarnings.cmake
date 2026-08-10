# Warning flags for first-party pp-browser targets (src/common, base, feature,
# app, render/libp2p integration). Does not apply to third_party/ or to the
# RmlUi / libp2p forks (those use their own options).

option(PP_BROWSER_WARNINGS_AS_ERRORS
  "Treat compiler warnings as errors for first-party pp-browser targets" ON)

function(pp_browser_apply_warnings target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "pp_browser_apply_warnings: target '${target}' does not exist")
  endif()

  get_target_property(_pp_warn_type ${target} TYPE)
  if(_pp_warn_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()

  if(MSVC)
    # Match GCC/Clang policy under /WX:
    # - CRT secure deprecations (getenv/localtime) — portable code uses the ISO APIs
    # - C4100 unused parameter — same as -Wno-unused-parameter
    # - C4458 hide class member — GCC does not enable -Wshadow by default
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /wd4100 /wd4458)
    if(PP_BROWSER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    # From -Wextra, suppress the two that are chronically noisy in this tree:
    # - missing-field-initializers: aggregate / designated init omitting fields
    # - unused-parameter: interface overrides and callback stubs
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wno-missing-field-initializers
      -Wno-unused-parameter)
    if(PP_BROWSER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
