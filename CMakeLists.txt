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
include(Progress)
find_package(Threads REQUIRED)
pp_configure_status("Configuring in-tree RmlUi (src/render)...")

# Hard-forked RmlUi (src/render/)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)

add_subdirectory(src/render EXCLUDE_FROM_ALL)
pp_configure_status("RmlUi configured; setting up pp-browser targets...")

if(PP_BROWSER_ENABLE_LIBP2P_BUILD)
  pp_configure_status("Configuring in-tree libp2p (src/libp2p)...")
  set(PACKAGE_MANAGER vendored CACHE STRING "" FORCE)
  set(TESTING OFF CACHE BOOL "" FORCE)
  set(EXAMPLES OFF CACHE BOOL "" FORCE)
  set(CLANG_TIDY OFF CACHE BOOL "" FORCE)
  set(CLANG_FORMAT OFF CACHE BOOL "" FORCE)
  add_subdirectory(src/libp2p EXCLUDE_FROM_ALL)
  pp_configure_status("libp2p configured")
endif()

set(PP_BROWSER_BACKEND_SOURCES
  src/render/integration/RmlUi_Platform_SDL.cpp
  src/render/integration/RmlUi_Renderer_GL3.cpp
  src/render/integration/BrowserHost.cpp
)

add_library(pp_rmlui_backend STATIC ${PP_BROWSER_BACKEND_SOURCES})
target_compile_definitions(pp_rmlui_backend PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
target_include_directories(pp_rmlui_backend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/Include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/Backends
  ${CMAKE_CURRENT_SOURCE_DIR}/src/render/integration
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
  src/app/Config.cpp
  src/app/InputCoordinator.cpp
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
  src/common/Module.cpp
  src/log/Logger.cpp
  src/platform/SequencedTaskRunner.cpp
  src/platform/BrowserThread.cpp
  src/messaging/IdUtil.cpp
  src/messaging/MessagingJson.cpp
  src/messaging/PeopleDiscoveryBlocks.cpp
  src/messaging/JsonThreadStore.cpp
  src/messaging/InboxController.cpp
  src/messaging/P2pMessagingService.cpp
  src/messaging/MessageRouter.cpp
  src/messaging/ContactActionDispatcher.cpp
  src/messaging/MessagingHub.cpp
  src/messaging/AtAiParser.cpp
  src/contacts/ContactsStore.cpp
  src/identity/Ed25519Signer.cpp
  src/identity/IdentityStore.cpp
  src/net/HttpClient.cpp
  src/net/ServiceClientsImpl.cpp
  src/demo/SearchDemo.cpp
  src/demo/ChatDemo.cpp
  src/demo/ChatFormHelper.cpp
  src/demo/ChatWidgetStateBuilder.cpp
  src/demo/CalendarHelper.cpp
  src/demo/DynamicRmlDemo.cpp
  src/ui/DocumentLoader.cpp
  src/ui/RmlMount.cpp
  src/ui/ShellLayout.cpp
  src/ui/ShellInterruption.cpp
  src/ui/ShellFeedback.cpp
  src/ui/ViewCatalog.cpp
  src/ui/ShellHost.cpp
  src/ui/Theme.cpp
  src/ui/DataModelHost.cpp
  src/bindings/ActionRouter.cpp
  src/bindings/BindingsManifest.cpp
  src/mcp/McpClient.cpp
  src/mcp/SchemaAdapter.cpp
  src/agent/LlmClient.cpp
  src/agent/PromptBuilder.cpp
  src/agent/StructuredTextParser.cpp
  src/agent/UiGenerator.cpp
  src/agent/RmlValidator.cpp
  src/agent/AgentSession.cpp
  src/agent/SearchIntent.cpp
  src/agent/conversation/Conversation.cpp
  src/agent/conversation/SlidingWindowContextPolicy.cpp
  src/agent/conversation/TurnCoordinator.cpp
  src/agent/conversation/UserMessageFormatter.cpp
  src/agent/conversation/ThreadContextPolicy.cpp
  src/agent/ToolRegistry.cpp
  src/agent/tools/WebSearchTool.cpp
  src/agent/tools/McpToolAdapter.cpp
  src/agent/tools/MessagingTools.cpp
)

target_link_libraries(pp-browser PRIVATE nlohmann_json::nlohmann_json CURL::libcurl Threads::Threads)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp-browser PRIVATE OpenSSL::Crypto)
endif()

pp_configure_status("Configuring tests...")
enable_testing()
add_subdirectory(tests)
pp_configure_status("Configure step complete")

install(TARGETS pp-browser RUNTIME DESTINATION bin)
install(DIRECTORY assets/ DESTINATION share/pp-browser/assets)
