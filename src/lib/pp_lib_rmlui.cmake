# RmlUi product embedding policy (product backend lives in src/base/render).
# RmlUi unit tests follow PP_BROWSER_BUILD_TESTS (no separate PP_BROWSER_RMLUI_TESTS).
# MSVC does not reliably resolve PRIVATE static-lib deps (rmlui_core -> lunasvg).

include(pp_lib_paths)

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

# Fixed product profile for the in-tree RmlUi fork (not user options).
function(pp_lib_apply_rmlui_product_profile)
  pp_configure_status("Configuring in-tree RmlUi (src/lib/rmlui)...")

  # Static embedding; no samples / Lua / Lottie.
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
  set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
  set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
  set(RMLUI_FONT_ENGINE_HARFBUZZ ON CACHE BOOL "HarfBuzz text shaping for RmlUi font engine" FORCE)

  if(PP_BROWSER_BUILD_TESTS)
    set(RMLUI_TESTS ON CACHE BOOL "Build in-tree RmlUi unit tests" FORCE)
    set(RMLUI_BACKEND "SDL_GL3" CACHE STRING "" FORCE)
    set(RMLUI_SDL_VERSION_MAJOR "3" CACHE STRING "" FORCE)
  else()
    set(RMLUI_TESTS OFF CACHE BOOL "Build in-tree RmlUi unit tests" FORCE)
  endif()
endfunction()
