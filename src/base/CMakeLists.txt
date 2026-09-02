include(PpBrowserBase)

# Thin aggregate: foundation + Amp L1–L3 aliases (FetchContent pp-cpp-amp).
# Product mesh peer lives under src/domain/mesh/ (pp_domain_mesh).
# AMP L1–L3 + link: cmake/PpCppAmp.cmake.

if(PP_BROWSER_HEADLESS)
  # Node-only: still expose foundation + amp aliases via pp_base for pp-node.
endif()

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
)

add_library(pp_base INTERFACE)
target_link_libraries(pp_base INTERFACE ${PP_BASE_MODULE_LIBS})

add_library(pp_identity ALIAS pp_base)
