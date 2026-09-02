include(PpBrowserBase)

# Acyclic module order: each library links only lower layers.
# foundation bands (runtime/platform/error/i18n/data/crypto/identity) live under src/foundation/.
# people + media + net + ui live under src/domain/; remaining peers still here.
# AMP L1–L3 + link: FetchContent pp-cpp-amp (cmake/PpCppAmp.cmake).
add_subdirectory(mesh)

if(PP_BROWSER_HEADLESS)
  # Node-only: no messaging/ai/ui aggregate.
  return()
endif()

add_subdirectory(messaging)
add_subdirectory(ai)

# Foundation + migrated domain targets listed here so pp_base still aggregates
# the full product stack until dedicated INTERFACE libs land.
set(PP_FOUNDATION_MODULE_LIBS
  pp_foundation_error
  pp_foundation_runtime_core
  pp_foundation_runtime
  pp_foundation_platform_core
  pp_foundation_platform
  pp_foundation_data
  pp_foundation_i18n
  pp_foundation_crypto
  pp_foundation_identity
)

set(PP_BASE_MODULE_LIBS
  ${PP_FOUNDATION_MODULE_LIBS}
  pp_base_adp
  pp_base_mesh_session
  pp_base_mesh_channel
  pp_base_mesh_link
  pp_base_mesh
  pp_base_messaging
  pp_base_ai
  pp_base_ai_conversation
  pp_base_ai_mcp
)

add_library(pp_base INTERFACE)
target_link_libraries(pp_base INTERFACE ${PP_BASE_MODULE_LIBS})

add_library(pp_identity ALIAS pp_base)
