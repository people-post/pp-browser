cmake_minimum_required(VERSION 3.24)
project(pp-browser VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(dependencies)

# Hard-forked RmlUi (thirdparty/rmlui/)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)

add_subdirectory(thirdparty/rmlui EXCLUDE_FROM_ALL)

set(PP_BROWSER_BACKEND_SOURCES
  src/render/backends/RmlUi_Platform_SDL.cpp
  src/render/backends/RmlUi_Renderer_GL3.cpp
  src/render/backends/BrowserHost.cpp
)

add_library(pp_rmlui_backend STATIC ${PP_BROWSER_BACKEND_SOURCES})
target_compile_definitions(pp_rmlui_backend PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
target_include_directories(pp_rmlui_backend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rmlui/Include
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rmlui/Backends
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/backends
)
target_link_libraries(pp_rmlui_backend PUBLIC
  RmlUi::Core
  ${PP_BROWSER_SDL3_TARGET}
  ${PP_BROWSER_SDL3_IMAGE_TARGET}
)
if(UNIX AND NOT APPLE)
  target_link_libraries(pp_rmlui_backend PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(pp-browser
  src/main.cpp
  src/app/Application.cpp
)

target_compile_definitions(pp-browser PRIVATE
  RMLUI_STATIC_LIB
  PP_BROWSER_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)

target_include_directories(pp-browser PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pp-browser PRIVATE
  pp_rmlui_backend
  RmlUi::Core
)

option(PPBROWSER_ENABLE_DEBUGGER "Link RmlUi debugger" ON)
option(PPBROWSER_LOCAL_LLM "Enable local LLM backend (not implemented)" OFF)
if(PPBROWSER_LOCAL_LLM)
  message(STATUS "PPBROWSER_LOCAL_LLM requested; implementation deferred")
endif()
if(PPBROWSER_ENABLE_DEBUGGER)
  target_link_libraries(pp-browser PRIVATE RmlUi::Debugger)
  target_compile_definitions(pp-browser PRIVATE PPBROWSER_ENABLE_DEBUGGER=1)
endif()

# Phase 2+
target_sources(pp-browser PRIVATE
  src/demo/SearchDemo.cpp
  src/ui/DocumentLoader.cpp
  src/ui/Theme.cpp
  src/ui/DataModelHost.cpp
  src/bindings/ActionRouter.cpp
  src/bindings/BindingsManifest.cpp
  src/mcp/McpClient.cpp
  src/mcp/SchemaAdapter.cpp
  src/agent/LlmClient.cpp
  src/agent/PromptBuilder.cpp
  src/agent/UiGenerator.cpp
  src/agent/RmlValidator.cpp
)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)
target_link_libraries(pp-browser PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)

enable_testing()
add_subdirectory(tests)

install(TARGETS pp-browser RUNTIME DESTINATION bin)
install(DIRECTORY assets/ DESTINATION share/pp-browser/assets)
