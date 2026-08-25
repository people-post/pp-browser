# Product RmlUi SDL/GL backend (base layer). Fork lives in src/lib/rmlui.

include(pp_lib_paths)
include(pp_lib_rmlui)

pp_browser_add_base_library(pp_base_render
  SOURCES
    platform/RmlUi_Platform_SDL.cpp
    platform/MobileGlLifecycle.cpp
    renderer/RmlUi_Renderer_GL3.cpp
    renderer/TextLoupeRenderer.cpp
    renderer/CallVideoTileRenderer.cpp
    renderer/ElementCallVideoTile.cpp
    host/BrowserHost.cpp
    host/TouchSimOverlay.cpp
  PUBLIC_LIBS
    RmlUi::Core
    ${PP_BROWSER_SDL3_TARGET}
    ${PP_BROWSER_SDL3_IMAGE_TARGET})

target_compile_definitions(pp_base_render PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
if(RMLUI_BACKEND_SIMULATE_TOUCH)
  target_compile_definitions(pp_base_render PUBLIC RMLUI_BACKEND_SIMULATE_TOUCH)
endif()
if(PP_BROWSER_IS_IOS)
  # OpenGLES is deprecated on iOS 12+; we still use ES3 via SDL until Metal.
  target_compile_definitions(pp_base_render PRIVATE GLES_SILENCE_DEPRECATION=1)
endif()

target_include_directories(pp_base_render PUBLIC
  ${PP_LIB_RMLUI_INCLUDE}
  ${CMAKE_CURRENT_SOURCE_DIR}/platform
  ${CMAKE_CURRENT_SOURCE_DIR}/renderer
  ${CMAKE_CURRENT_SOURCE_DIR}/host
)

pp_browser_link_rmlui_svg_deps(pp_base_render)
if(UNIX AND NOT APPLE)
  target_link_libraries(pp_base_render PUBLIC ${CMAKE_DL_LIBS})
endif()
if(ANDROID)
  target_link_libraries(pp_base_render PUBLIC log GLESv3 EGL)
endif()
