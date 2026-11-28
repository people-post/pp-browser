# Source layers: common → foundation → lib → domain → feature → gui → app.
# Convenience INTERFACEs `pp_foundation` and `pp_base` = foundation + Amp + mesh + lib: the app's and its tools'
# (capabilities name what they link; check_capability_edges.py, CR016).
# The base (pp-cpp-core's capabilities/base, CR023 step 3), then the app's own common layer and the product's base.
add_subdirectory(${PP_CPP_CORE_DIR}/capabilities/base ${CMAKE_BINARY_DIR}/capabilities/base)
# The headless product base both apps use (pp-cpp-core, CR028); the app's own on top of it in foundation/.
add_subdirectory(${PP_CPP_CORE_DIR}/capabilities/product ${CMAKE_BINARY_DIR}/capabilities/product)
add_subdirectory(common/ui)
add_subdirectory(foundation)
# Capabilities moved out of src/ (projects/capability-repos CR022), in the capability order: people, net and node from
# pp-cpp-core (cmake/PpCore.cmake), the rest this tree's.
add_subdirectory(${PP_CPP_CORE_DIR}/capabilities/people ${CMAKE_BINARY_DIR}/capabilities/people)
add_subdirectory(${PP_CPP_CORE_DIR}/capabilities/net ${CMAKE_BINARY_DIR}/capabilities/net)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/social ${CMAKE_BINARY_DIR}/capabilities/social)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/delivery ${CMAKE_BINARY_DIR}/capabilities/delivery)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/pricing ${CMAKE_BINARY_DIR}/capabilities/pricing)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/media ${CMAKE_BINARY_DIR}/capabilities/media)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/messaging ${CMAKE_BINARY_DIR}/capabilities/messaging)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/calls ${CMAKE_BINARY_DIR}/capabilities/calls)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/broadcast ${CMAKE_BINARY_DIR}/capabilities/broadcast)
add_subdirectory(${PP_CPP_CORE_DIR}/capabilities/node ${CMAKE_BINARY_DIR}/capabilities/node)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/relay ${CMAKE_BINARY_DIR}/capabilities/relay)
add_subdirectory(${CMAKE_SOURCE_DIR}/capabilities/ledger ${CMAKE_BINARY_DIR}/capabilities/ledger)
add_subdirectory(lib)

# Convenience INTERFACE: all foundation modules, the base's and the product's (not Amp — Amp is FetchContent lib).
add_library(pp_foundation INTERFACE)
target_link_libraries(pp_foundation INTERFACE
  pp_foundation_error
  pp_foundation_runtime_core
  pp_foundation_runtime
  pp_foundation_platform_core
  pp_foundation_tls
  pp_foundation_platform
  pp_foundation_data
  pp_foundation_diagnostics
  pp_foundation_i18n
  pp_foundation_crypto
  pp_foundation_identity
  pp_foundation_product
)

# Product-stack convenience (legacy name). Prefer linking specific pp_foundation_* /
# pp_lib_* / pp_amp_* / pp_mesh_* targets when a narrower dependency is enough.
add_library(pp_base INTERFACE)
target_link_libraries(pp_base INTERFACE
  pp_foundation
  pp_amp_l1
  pp_amp_l2
  pp_amp_l3
  pp_amp_link
  pp_mesh
  pp_lib
)

# Domain: capabilities composed from libs, shared by features (and pp-node / the probes).
add_subdirectory(domain)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(feature)
  add_subdirectory(gui)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
endif()

add_subdirectory(app)
