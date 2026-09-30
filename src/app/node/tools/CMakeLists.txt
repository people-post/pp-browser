# Lab / smoke tools beside pp-node. Sources live here so node code and test tooling are easy to
# tell apart; binaries still land in the pp-node build dir (scripts / hard lab mount it as /probes).
include(PpBrowserWarnings)
set(_pp_node_tools_out "${CMAKE_BINARY_DIR}/src/app/node")

# Amp thin-client probes (A017) — ephemeral AmpStack, no Libp2pHost.
set(_pp_probe_libs
  pp_domain_mesh
  pp_amp_link
  pp_foundation_crypto
  pp_amp_l1
  pp_pbr_common
  Threads::Threads
  sodium
)
if(TARGET OpenSSL::Crypto)
  list(APPEND _pp_probe_libs OpenSSL::Crypto)
endif()

add_executable(pp-node-probe probe/main.cpp)
target_include_directories(pp-node-probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp-node-probe PRIVATE ${_pp_probe_libs})
pp_browser_apply_warnings(pp-node-probe)
set_target_properties(pp-node-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_pp_node_tools_out}")
install(TARGETS pp-node-probe RUNTIME DESTINATION bin)

# call_probe needs AmpDirectChatTransport (--with-chat); skip in headless (no messaging).
if(TARGET pp_feature_conversations OR TARGET pp_domain_messaging)
  add_executable(pp-call-probe
    call_probe/main.cpp
    call_probe/ProductStackHarness.cpp
  )
  target_include_directories(pp-call-probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(pp-call-probe PRIVATE
    ${_pp_probe_libs}
    pp_foundation_platform_core
  )
  # Phase-2 NAT + HL004 product-stack: CallStack / AmpCircuitHopReach in pp_feature_calls.
  if(TARGET pp_feature_calls)
    target_link_libraries(pp-call-probe PRIVATE pp_feature_calls)
  endif()
  if(TARGET pp_feature_conversations)
    target_link_libraries(pp-call-probe PRIVATE pp_feature_conversations)
  else()
    target_sources(pp-call-probe PRIVATE
      ${CMAKE_SOURCE_DIR}/src/feature/conversations/AmpDirectChatTransport.cpp)
    target_link_libraries(pp-call-probe PRIVATE pp_domain_messaging)
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
      pp_feature_calls
      pp_feature_conversations
      GTest::gtest
      GTest::gtest_main
    )
    pp_browser_apply_warnings(pp_browser_call_probe_test)
    include(PpBrowserTesting)
    pp_browser_add_gtest(pp_browser_call_probe_test)
  endif()
endif()
