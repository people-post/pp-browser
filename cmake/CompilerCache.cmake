# Optional compiler cache: ccache on Unix-like hosts, sccache on Windows (MSVC).
#
# Enable with -DPP_BROWSER_COMPILER_CACHE=ON before or during configure.
# Launchers must be set before project(); MSVC debug-format tweaks run after.

option(PP_BROWSER_COMPILER_CACHE "Use sccache (Windows) or ccache (elsewhere) when available" OFF)

function(pp_browser_configure_compiler_cache)
  if(NOT PP_BROWSER_COMPILER_CACHE)
    return()
  endif()

  if(WIN32)
    find_program(PP_BROWSER_COMPILER_CACHE_BIN sccache)
    set(PP_BROWSER_COMPILER_CACHE_NAME "sccache")
  else()
    find_program(PP_BROWSER_COMPILER_CACHE_BIN ccache)
    set(PP_BROWSER_COMPILER_CACHE_NAME "ccache")
  endif()

  if(NOT PP_BROWSER_COMPILER_CACHE_BIN)
    message(WARNING "PP_BROWSER_COMPILER_CACHE=ON but ${PP_BROWSER_COMPILER_CACHE_NAME} was not found in PATH")
    return()
  endif()

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

  # sccache cannot cache MSVC compiles that write separate PDBs (/Zi).
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.25")
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>"
        CACHE STRING "MSVC debug info format for sccache compatibility" FORCE)
  else()
    foreach(_flag_var
        CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      string(REPLACE "/Zi" "/Z7" ${_flag_var} "${${_flag_var}}")
    endforeach()
  endif()
endfunction()
