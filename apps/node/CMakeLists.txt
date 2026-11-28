# pp-node, the headless node (capability-repos CR017, CR028): its two builds (pp-node, pp-node-plus), its tests and its lab
# tools. pp-browser's configure adds this folder (src/app/CMakeLists.txt), its binaries under build/src/app/node as
# before; it also configures on its own, without src/ (CR028 step 3):
#
#   cmake -S apps/node -B build-node -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-node --target pp-node pp-node-plus pp_browser_pp_node_test
#   ctest --test-dir build-node
#
# On its own it builds the capabilities it links from capabilities/ (in pp-node's own repo, from pp-cpp-core, CR027),
# headless, with the same pins. Linux and macOS.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.24)
  get_filename_component(_pp_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  list(APPEND CMAKE_MODULE_PATH "${_pp_root}/cmake")
  include(CompilerCache)
  pp_browser_configure_compiler_cache()
  project(pp-node LANGUAGES CXX C)
  if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "iOS|Android")
    message(FATAL_ERROR "pp-node builds on its own on Linux and macOS: build ${CMAKE_SYSTEM_NAME} through the app")
  endif()
  set(PP_BROWSER_HEADLESS ON CACHE BOOL "Headless: pp-node only" FORCE)
  set(PP_CAPABILITIES_DIR "${_pp_root}/capabilities")
  set(PP_APP_ROOT "${_pp_root}") # pp-node's relay certificates and commit are pp-browser's tree's until it moves
  set(PP_CAPABILITIES_DEFAULT "product;node;relay;ledger")
  include(PpCapabilitiesRoot)
endif()

if(PP_BROWSER_IS_MOBILE)
  return()
endif()

include(PpBrowserWarnings)
include(PpBrowserNoUiDeps)

# The node app's sources but main: both editions link them (projects/node-relay NR001).
add_library(pp_app_node STATIC
  src/app/node/NodeBootstrap.cpp
  src/app/node/NodeCompatFloor.cpp
  src/app/node/NodeConfigLoad.cpp
  src/app/node/NodeEnvOverlay.cpp
  src/app/node/NodeServiceAsks.cpp
  src/app/node/NodeHeartbeat.cpp
  src/app/node/NodeMeshPublish.cpp
  src/app/node/NodeMetrics.cpp
  src/app/node/NodeUpgradeNotice.cpp
  src/app/node/StatusHttpProtocol.cpp
  src/app/node/StatusHttpServer.cpp
)
# The node's own include root ("app/node/…"): never src/, so the node reaches the desktop app's code by no path.
target_include_directories(pp_app_node PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pp_app_node PUBLIC
  pp_domain_registration
  pp_domain_node
  pp_mesh
  pp_lib_people
  pp_lib_net
  pp_foundation_crypto
  pp_product_data
  pp_product_platform
  pp_foundation_product
  pp_foundation_runtime_core
  pp_foundation_error
  pp_pbr_common
  pp_foundation_sockets
  Threads::Threads
  sodium
  CURL::libcurl
)
pp_browser_apply_warnings(pp_app_node)

# The two node binaries, each with its set of services (cmake/PpNodeServices.cmake; projects/node-relay NR001):
# pp-node, what everyone who helps the mesh runs (the ledger role), and pp-node-plus, with the add-ons (the relay).
# -DPP_NODE_SERVICES=… / -DPP_NODE_PLUS_SERVICES=… build either with another set. A desktop tree builds pp-node-plus only
# when asked (--target pp-node-plus); the headless build (the package script, CI's node-editions job) builds both.
include(PpNodeServices)
pp_add_node_binary(pp-node "${PP_NODE_SERVICES}")
if(PP_BROWSER_HEADLESS)
  pp_add_node_binary(pp-node-plus "${PP_NODE_PLUS_SERVICES}")
else()
  pp_add_node_binary(pp-node-plus "${PP_NODE_PLUS_SERVICES}" EXCLUDE_FROM_ALL)
endif()

install(TARGETS pp-node RUNTIME DESTINATION bin)

# Lab / smoke tools (not shipped in the node image): the Amp probe and the stress lab's load generator. The app's
# probes (pp-app-probe, pp-call-probe) are the app's, in src/app/probes.
add_subdirectory(src/app/node/tools)

if(PP_BROWSER_BUILD_TESTS)
  add_executable(pp_browser_pp_node_test
    src/app/node/tests/status_http_protocol_test.cpp
    src/app/node/tests/node_config_test.cpp
    src/app/node/tests/node_env_overlay_test.cpp
    src/app/node/tests/node_service_asks_test.cpp
    src/app/node/tests/node_heartbeat_test.cpp
    src/app/node/tests/status_http_server_test.cpp
    src/app/node/tests/node_upgrade_notice_test.cpp
    src/app/node/tests/node_compat_floor_test.cpp
    src/app/node/NodeCompatFloor.cpp
    src/app/node/NodeHeartbeat.cpp
    src/app/node/NodeUpgradeNotice.cpp
    src/app/node/StatusHttpProtocol.cpp
    src/app/node/StatusHttpServer.cpp
    src/app/node/NodeConfigLoad.cpp
    src/app/node/NodeEnvOverlay.cpp
    src/app/node/NodeServiceAsks.cpp
  )
  target_include_directories(pp_browser_pp_node_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(pp_browser_pp_node_test PRIVATE
    GTest::gtest
    GTest::gtest_main
    pp_product_data
    pp_pbr_common
    pp_foundation_tls
    pp_lib_net
    pp_foundation_sockets
    Threads::Threads
  )
  pp_browser_apply_warnings(pp_browser_pp_node_test)
  include(PpBrowserTesting)
  pp_browser_add_gtest(pp_browser_pp_node_test)

  # The node's loader against the app's (node_config_app_test): only where the app's config is built.
  if(TARGET pp_foundation_data)
    add_executable(pp_browser_pp_node_config_app_test
      src/app/node/tests/node_config_app_test.cpp
      src/app/node/NodeConfigLoad.cpp)
    target_include_directories(pp_browser_pp_node_config_app_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(pp_browser_pp_node_config_app_test PRIVATE
      GTest::gtest
      GTest::gtest_main
      pp_foundation_data
      pp_pbr_common)
    pp_browser_apply_warnings(pp_browser_pp_node_config_app_test)
    pp_browser_add_gtest(pp_browser_pp_node_config_app_test)
  endif()
endif()
