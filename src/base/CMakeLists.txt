include(PpBrowserBase)

# Acyclic module order: each library links only lower layers.
# runtime before platform: OS adapters may use AppLifecycle / AppRuntime.
add_subdirectory(runtime)
add_subdirectory(platform)
add_subdirectory(i18n)
add_subdirectory(error)
add_subdirectory(data)
add_subdirectory(crypto)
# AMP L1–L3 live under src/lib/amp (future pp-cpp-amp); built here after crypto/error.
add_subdirectory(${CMAKE_SOURCE_DIR}/src/lib/amp ${CMAKE_BINARY_DIR}/src/lib/amp)
add_subdirectory(p2p)
add_subdirectory(people)

if(PP_BROWSER_HEADLESS)
  # Node-only: no messaging/media/net/ai/ui/render aggregate.
  return()
endif()

add_subdirectory(messaging)
add_subdirectory(media)
add_subdirectory(net)
add_subdirectory(ai)
add_subdirectory(ui)
add_subdirectory(render)

set(PP_BASE_MODULE_LIBS
  pp_base_error
  pp_base_runtime_core
  pp_base_runtime
  pp_base_platform_core
  pp_base_platform
  pp_base_data
  pp_base_i18n
  pp_base_crypto
  pp_base_adp
  pp_base_mesh_session
  pp_base_mesh_channel
  pp_base_mesh_link
  pp_base_peer_id
  pp_base_p2p
  pp_base_people
  pp_base_messaging
  pp_base_media
  pp_base_net
  pp_base_ai
  pp_base_ai_conversation
  pp_base_ai_mcp
  pp_base_ui
  pp_base_render
)

add_library(pp_base INTERFACE)
target_link_libraries(pp_base INTERFACE ${PP_BASE_MODULE_LIBS})

add_library(pp_identity ALIAS pp_base)
