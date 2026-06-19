# MSVC does not reliably resolve PRIVATE static-lib deps (rmlui_core -> lunasvg).
# App/test glue only; keep the RmlUi fork unchanged.
function(pp_browser_link_rmlui_core target)
  target_link_libraries(${target} PRIVATE RmlUi::Core)
  if(WIN32 AND TARGET lunasvg::lunasvg)
    target_link_libraries(${target} PRIVATE lunasvg::lunasvg)
  endif()
endfunction()

function(pp_browser_link_rmlui_svg_deps target)
  if(WIN32 AND TARGET lunasvg::lunasvg)
    target_link_libraries(${target} PRIVATE lunasvg::lunasvg)
  endif()
endfunction()
