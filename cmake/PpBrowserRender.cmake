# RmlUi fork configuration and pp-browser render integration glue.
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

function(pp_browser_configure_rmlui_fork)
  pp_configure_status("Configuring in-tree RmlUi (src/render)...")

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

function(pp_browser_add_rmlui_backend)
  set(_pp_rmlui_backend_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/platform/RmlUi_Platform_SDL.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/platform/MobileGlLifecycle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/renderer/RmlUi_Renderer_GL3.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/renderer/TextLoupeRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/host/BrowserHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/host/TouchSimOverlay.cpp
  )

  add_library(pp_rmlui_backend STATIC ${_pp_rmlui_backend_sources})
  target_compile_definitions(pp_rmlui_backend PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
  if(RMLUI_BACKEND_SIMULATE_TOUCH)
    target_compile_definitions(pp_rmlui_backend PUBLIC RMLUI_BACKEND_SIMULATE_TOUCH)
  endif()
  target_include_directories(pp_rmlui_backend PUBLIC
    ${CMAKE_SOURCE_DIR}/src/render/fork/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/host
  )
  target_link_libraries(pp_rmlui_backend PUBLIC
    RmlUi::Core
    ${PP_BROWSER_SDL3_TARGET}
    ${PP_BROWSER_SDL3_IMAGE_TARGET}
  )
  pp_browser_link_rmlui_svg_deps(pp_rmlui_backend)
  if(UNIX AND NOT APPLE)
    target_link_libraries(pp_rmlui_backend PUBLIC ${CMAKE_DL_LIBS})
  endif()
  if(ANDROID)
    target_link_libraries(pp_rmlui_backend PUBLIC log GLESv3 EGL)
  endif()
endfunction()
