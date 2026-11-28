# The app's probes (the hard lab and the app lab drive them): the product stack with no GUI. pp-call-probe runs it over
# real Amp links, pp-app-probe is the app itself, one scripted user per process. They need the app's code, so they are
# the app's (capability-repos CR028), not the node's; their binaries land beside pp-node's as before (the labs mount
# that dir as /probes).
include(PpBrowserWarnings)
set(_pp_node_tools_out "${CMAKE_BINARY_DIR}/src/app/node")

set(_pp_probe_libs
  pp_mesh
  pp_amp_link
  pp_foundation_crypto
  pp_amp_l1
  pp_pbr_common
  Threads::Threads
  sodium
)

# call_probe needs AmpDirectChatTransport (--with-chat); skip in headless (no messaging).
if(TARGET pp_feature_conversations OR TARGET pp_lib_messaging)
  add_executable(pp-call-probe
    call_probe/main.cpp
    call_probe/ProductStackHarness.cpp
  )
  target_include_directories(pp-call-probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
  # The harness builds what the app does (the profile's mesh config, the OS network monitor); the feature modules it
  # links name only their capability's needs (CR022), so it names the product's base itself.
  target_link_libraries(pp-call-probe PRIVATE
    ${_pp_probe_libs}
    pp_foundation_platform_core
    pp_foundation_platform
    pp_foundation_data
    pp_lib_storage
    pp_domain_call_media
  )
  # Phase-2 NAT + HL004 product-stack: CallStack / AmpCircuitHopReach in pp_feature_calls.
  if(TARGET pp_feature_calls)
    target_link_libraries(pp-call-probe PRIVATE pp_feature_calls pp_feature_broadcast)
  endif()
  if(TARGET pp_feature_conversations)
    target_link_libraries(pp-call-probe PRIVATE pp_feature_conversations)
  else()
    target_sources(pp-call-probe PRIVATE
      ${CMAKE_SOURCE_DIR}/capabilities/delivery/src/domain/delivery/AmpDirectChatTransport.cpp)
    target_link_libraries(pp-call-probe PRIVATE pp_lib_messaging pp_lib_calls pp_lib_broadcast pp_lib_pricing)
  endif()
  pp_browser_apply_warnings(pp-call-probe)
  set_target_properties(pp-call-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_pp_node_tools_out}")
  install(TARGETS pp-call-probe RUNTIME DESTINATION bin)

  if(PP_BROWSER_BUILD_TESTS AND TARGET pp_feature_calls AND TARGET pp_feature_conversations)
    # Product stack over a real loopback Amp stack (the harness the hard lab drives).
    add_executable(pp_browser_call_probe_test
      tests/product_stack_harness_test.cpp
      call_probe/ProductStackHarness.cpp
    )
    target_include_directories(pp_browser_call_probe_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(pp_browser_call_probe_test PRIVATE
      ${_pp_probe_libs}
      pp_foundation_platform_core
      pp_foundation_platform
      pp_foundation_data
      pp_lib_storage
      pp_feature_calls
      pp_feature_broadcast
      pp_feature_conversations
      GTest::gtest
      GTest::gtest_main
    )
    pp_browser_apply_warnings(pp_browser_call_probe_test)
    include(PpBrowserTesting)
    pp_browser_add_gtest(pp_browser_call_probe_test)
  endif()
endif()

# pp-app-probe: the app with no GUI (same Bootstrap + ProductStack), one scripted user per process.
# Desktop configure only (needs the product feature stack).
if(TARGET pp_feature_product)
  add_executable(pp-app-probe
    app_probe/main.cpp
    ${CMAKE_SOURCE_DIR}/src/app/Bootstrap.cpp
  )
  target_include_directories(pp-app-probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(pp-app-probe PRIVATE
    pp_feature_product
    pp_feature
    pp_base
    pp_pbr_common
    pp_mesh
    CURL::libcurl
    Threads::Threads
  )
  pp_browser_apply_warnings(pp-app-probe)
  set_target_properties(pp-app-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_pp_node_tools_out}")
endif()
