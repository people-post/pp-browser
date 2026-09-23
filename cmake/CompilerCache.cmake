# Optional compiler cache: ccache everywhere, falling back to sccache on
# Windows when ccache isn't installed (e.g. local dev machines without choco).
#
# Enable with -DPP_BROWSER_COMPILER_CACHE=ON before or during configure.
# Launchers must be set before project(); MSVC debug-format tweaks run after.

option(PP_BROWSER_COMPILER_CACHE "Use ccache (falling back to sccache on Windows) when available" OFF)

function(pp_browser_configure_compiler_cache)
  if(NOT PP_BROWSER_COMPILER_CACHE)
    return()
  endif()

  if(WIN32)
    find_program(PP_BROWSER_COMPILER_CACHE_BIN NAMES ccache sccache)
  else()
    find_program(PP_BROWSER_COMPILER_CACHE_BIN ccache)
  endif()

  if(NOT PP_BROWSER_COMPILER_CACHE_BIN)
    message(WARNING "PP_BROWSER_COMPILER_CACHE=ON but no compiler cache was found in PATH")
    return()
  endif()

  get_filename_component(PP_BROWSER_COMPILER_CACHE_NAME "${PP_BROWSER_COMPILER_CACHE_BIN}" NAME_WE)

  set(CMAKE_C_COMPILER_LAUNCHER "${PP_BROWSER_COMPILER_CACHE_BIN}" CACHE STRING "C compiler launcher" FORCE)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${PP_BROWSER_COMPILER_CACHE_BIN}" CACHE STRING "CXX compiler launcher" FORCE)
  message(STATUS "Compiler cache: ${PP_BROWSER_COMPILER_CACHE_NAME} (${PP_BROWSER_COMPILER_CACHE_BIN})")
endfunction()

function(pp_browser_finalize_compiler_cache)
  if(NOT PP_BROWSER_COMPILER_CACHE)
    return()
  endif()
  if(NOT WIN32 OR NOT MSVC)
    return()
  endif()
  if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    return()
  endif()

  # Neither ccache nor sccache can cache MSVC compiles that write separate PDBs (/Zi).
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.25")
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>"
        CACHE STRING "MSVC debug info format for compiler cache compatibility" FORCE)
  else()
    foreach(_flag_var
        CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      string(REPLACE "/Zi" "/Z7" ${_flag_var} "${${_flag_var}}")
    endforeach()
  endif()
endfunction()
