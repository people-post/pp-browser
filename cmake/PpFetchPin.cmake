# Refresh a cached FetchContent GIT_TAG when the pin in CMake source changes,
# without clobbering an intentional -D override (tag != last recorded pin).
#
# Usage:
#   include(PpFetchPin)
#   pp_fetch_git_tag(PP_CPP_COMMON_GIT_TAG "v0.2.3"
#     "Release tag on pp-cpp-common main (not a branch name)")

function(pp_fetch_git_tag cache_var pin doc)
  set(_last_pin_var "${cache_var}_LAST_PIN")
  if(NOT DEFINED CACHE{${_last_pin_var}})
    # First configure with pin tracking (or wiped LAST_PIN): adopt the cmake pin.
    # Refreshes stale tags left by older builds that used plain CACHE STRING.
    set(${cache_var} "${pin}" CACHE STRING "${doc}" FORCE)
  elseif("${${cache_var}}" STREQUAL "${${_last_pin_var}}" AND
         NOT "${${cache_var}}" STREQUAL "${pin}")
    # Cache still holds the previous pin; cmake pin moved — follow it.
    set(${cache_var} "${pin}" CACHE STRING "${doc}" FORCE)
  else()
    # Unset → seed; otherwise keep cache (user override or already current).
    set(${cache_var} "${pin}" CACHE STRING "${doc}")
  endif()
  set(${_last_pin_var} "${pin}" CACHE INTERNAL
    "Last ${cache_var} pin from CMake source (auto-refresh)" FORCE)
endfunction()
