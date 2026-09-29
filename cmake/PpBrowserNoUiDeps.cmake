# Guard headless-safe targets (pp-node) against UI / SDL / RmlUi link edges.
#
# Walks LINK_LIBRARIES + INTERFACE_LINK_LIBRARIES at configure time so a bad
# PUBLIC_LIBS edge fails the generate step instead of silently bloating the
# binary. Pair with pp_browser_add_no_ui_link_check for a post-link nm scan.

function(_pp_browser_normalize_link_item item out_var)
  set(_item "${item}")
  # Drop common generator-expression wrappers we can evaluate statically.
  string(REGEX REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1" _item "${_item}")
  string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.+)>$" "\\1" _item "${_item}")
  string(REGEX REPLACE "^\\$<INSTALL_INTERFACE:.*>$" "" _item "${_item}")
  # Skip remaining unevaluated genex / flags / bare system libs.
  if(_item MATCHES "^\\$<" OR _item MATCHES "^-" OR _item STREQUAL "")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  set(${out_var} "${_item}" PARENT_SCOPE)
endfunction()

function(_pp_browser_is_forbidden_ui_dep dep out_var)
  set(_forbidden_targets
    pp_foundation_platform
    pp_foundation_runtime
    pp_gui
    pp_domain_ui
    pp_domain_media
    pp_ui
    pp_ui_core
    pp_ui_backend
    ui
    ui_debugger
    rmlui
    rmlui_debugger
    SDL3
    SDL3-static
    SDL3-shared
    SDL3_image
    SDL3_image-static
    SDL3_image-shared
    SDL_uclibc
  )
  set(_forbidden_aliases
    ui::core
    ui::debugger
    SDL3::SDL3
    SDL3::SDL3-static
    SDL3::SDL3-shared
    SDL3_image::SDL3_image
    SDL::SDL
    SDL_image::SDL_image
  )
  if(dep IN_LIST _forbidden_targets OR dep IN_LIST _forbidden_aliases)
    set(${out_var} TRUE PARENT_SCOPE)
    return()
  endif()
  # Catch FetchContent / alias spellings (e.g. SDL3::SDL3-static).
  if(dep MATCHES "(^|::)(ui|SDL3|SDL3_image|rmlui)(::|$)" OR dep MATCHES "pp_foundation_platform$" OR dep MATCHES "pp_gui$" OR dep MATCHES "pp_domain_ui$" OR dep MATCHES "pp_domain_media$")
    set(${out_var} TRUE PARENT_SCOPE)
    return()
  endif()
  set(${out_var} FALSE PARENT_SCOPE)
endfunction()

# Every link edge reachable from root_target, as "from -> to" (targets walked, genex dropped).
function(_pp_browser_collect_link_edges root_target out_var)
  set(_queue "${root_target}")
  set(_seen "")
  set(_edges "")

  while(_queue)
    list(POP_FRONT _queue _current)
    if(_current IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen "${_current}")

    if(NOT TARGET "${_current}")
      continue()
    endif()

    set(_props LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_type "${_current}" TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
      set(_props INTERFACE_LINK_LIBRARIES)
    endif()

    foreach(_prop IN LISTS _props)
      get_target_property(_libs "${_current}" ${_prop})
      if(NOT _libs OR _libs STREQUAL "NOTFOUND")
        continue()
      endif()
      foreach(_raw IN LISTS _libs)
        _pp_browser_normalize_link_item("${_raw}" _dep)
        if(_dep STREQUAL "")
          continue()
        endif()
        list(APPEND _edges "${_current} -> ${_dep}")
        if(TARGET "${_dep}" AND NOT _dep IN_LIST _seen)
          list(APPEND _queue "${_dep}")
        endif()
      endforeach()
    endforeach()
  endwhile()
  list(REMOVE_DUPLICATES _edges)
  set(${out_var} "${_edges}" PARENT_SCOPE)
endfunction()

function(pp_browser_assert_no_ui_dependencies root_target)
  if(NOT TARGET ${root_target})
    message(FATAL_ERROR "pp_browser_assert_no_ui_dependencies: not a target: ${root_target}")
  endif()

  _pp_browser_collect_link_edges(${root_target} _edges)
  set(_hits "")
  foreach(_edge IN LISTS _edges)
    string(REGEX REPLACE "^.* -> " "" _dep "${_edge}")
    _pp_browser_is_forbidden_ui_dep("${_dep}" _bad)
    if(_bad)
      list(APPEND _hits "${_edge}")
    endif()
  endforeach()

  if(_hits)
    string(JOIN "\n  " _hit_text ${_hits})
    message(FATAL_ERROR
      "${root_target} must stay UI-free (no SDL / RmlUi / pp_gui / pp_foundation_platform).\n"
      "Forbidden link edges:\n  ${_hit_text}\n"
      "Use pp_foundation_platform_core (and other *_core targets) instead. "
      "See docs/ops/BUILD.md (Headless mesh node).")
  endif()
endfunction()

# pp-node's first-party closure: foundation (headless *_core), common, the node's domain peers
# (mesh / people / net), headless feature modules, and the Amp / common / crypto siblings. Any other
# pp_* target reachable from root_target fails configure — widening the node is a deliberate edit here.
set(PP_BROWSER_NODE_LINK_ALLOWLIST
  "^pp_foundation_(crypto|data|error|i18n|identity|platform_core|runtime_core)$"
  "^pp_domain_(mesh|people|net)$"
  "^pp_feature_(registration|node)$"
  "^pp_pbr_common$"
  "^pp_(common|crypto)$"
  "^pp_amp_")

function(pp_browser_assert_node_link_allowlist root_target)
  if(NOT TARGET ${root_target})
    message(FATAL_ERROR "pp_browser_assert_node_link_allowlist: not a target: ${root_target}")
  endif()

  _pp_browser_collect_link_edges(${root_target} _edges)
  set(_hits "")
  foreach(_edge IN LISTS _edges)
    string(REGEX REPLACE "^.* -> " "" _dep "${_edge}")
    string(REGEX REPLACE "^.*::" "" _name "${_dep}")
    if(NOT _name MATCHES "^pp_")
      continue()
    endif()
    set(_ok FALSE)
    foreach(_re IN LISTS PP_BROWSER_NODE_LINK_ALLOWLIST)
      if(_name MATCHES "${_re}")
        set(_ok TRUE)
        break()
      endif()
    endforeach()
    if(NOT _ok)
      list(APPEND _hits "${_edge}")
    endif()
  endforeach()

  if(_hits)
    string(JOIN "\n  " _hit_text ${_hits})
    message(FATAL_ERROR
      "${root_target} links first-party targets outside the node allowlist.\n"
      "Link edges:\n  ${_hit_text}\n"
      "The node may link foundation *_core, common, pp_domain_{mesh,people,net}, headless "
      "pp_feature_{registration,node} and Amp. See PP_BROWSER_NODE_LINK_ALLOWLIST in "
      "cmake/PpBrowserNoUiDeps.cmake and docs/ops/BUILD.md (Headless mesh node).")
  endif()
endfunction()

# Post-link scan: fails if UI entrypoints landed in the binary despite CMake edges.
# Windows/MSVC: prefer dumpbin — Strawberry/MinGW nm often cannot read ARM64 PE
# ("file format not recognized") and would false-fail the build after a good link.
function(pp_browser_add_no_ui_link_check target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "pp_browser_add_no_ui_link_check: not a target: ${target}")
  endif()
  if(CMAKE_CROSSCOMPILING)
    return()
  endif()

  set(_pp_symtool "")
  set(_pp_symtool_kind "")
  if(WIN32)
    find_program(_pp_dumpbin NAMES dumpbin)
    if(_pp_dumpbin)
      set(_pp_symtool "${_pp_dumpbin}")
      set(_pp_symtool_kind "dumpbin")
    else()
      message(STATUS
        "pp-browser: dumpbin not found; skipping post-link UI symbol check for ${target} "
        "(do not use Strawberry/MinGW nm on Windows PE)")
      return()
    endif()
  else()
    find_program(_pp_nm NAMES llvm-nm nm)
    if(NOT _pp_nm)
      message(STATUS "pp-browser: nm not found; skipping post-link UI symbol check for ${target}")
      return()
    endif()
    set(_pp_symtool "${_pp_nm}")
    set(_pp_symtool_kind "nm")
  endif()

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DPP_BROWSER_NO_UI_BINARY=$<TARGET_FILE:${target}>
      -DPP_BROWSER_NO_UI_SYMTOOL=${_pp_symtool}
      -DPP_BROWSER_NO_UI_SYMTOOL_KIND=${_pp_symtool_kind}
      -DPP_BROWSER_NO_UI_LABEL=${target}
      -P ${CMAKE_SOURCE_DIR}/cmake/CheckNoUiSymbols.cmake
    COMMENT "Checking ${target} for UI/SDL symbols"
    VERBATIM)
endfunction()
