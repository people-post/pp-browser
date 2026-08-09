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
    target_compile_options(${target} PRIVATE /W4)
    if(PP_BROWSER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(PP_BROWSER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
