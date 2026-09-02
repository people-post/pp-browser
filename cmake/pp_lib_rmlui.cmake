# Compatibility wrappers — RmlUi link helpers now live in cmake/PpCppUi.cmake
# (populated when PpCppUi is included). Kept so legacy `include(pp_lib_rmlui)` still works.

if(NOT COMMAND pp_browser_link_rmlui_core)
  function(pp_browser_link_rmlui_core target)
    target_link_libraries(${target} PRIVATE RmlUi::Core)
    if(WIN32 AND TARGET lunasvg::lunasvg)
      target_link_libraries(${target} PRIVATE lunasvg::lunasvg)
    endif()
  endfunction()
endif()

if(NOT COMMAND pp_browser_link_rmlui_svg_deps)
  function(pp_browser_link_rmlui_svg_deps target)
    if(WIN32 AND TARGET lunasvg::lunasvg)
      target_link_libraries(${target} PRIVATE lunasvg::lunasvg)
    endif()
  endfunction()
endif()
