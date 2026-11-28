# The node's lab / smoke tools (not shipped in the node image). Their binaries land in the pp-node build dir (scripts and
# the hard lab mount it as /probes). The app's probes (pp-app-probe, pp-call-probe) are in src/app/probes.
include(PpBrowserWarnings)
set(_pp_node_tools_out "${CMAKE_BINARY_DIR}/src/app/node")

# Amp thin-client probes (A017) — ephemeral AmpStack, no Libp2pHost.
set(_pp_probe_libs
  pp_mesh
  pp_amp_link
  pp_foundation_crypto
  pp_amp_l1
  pp_pbr_common
  Threads::Threads
  sodium
)

add_executable(pp-node-probe probe/main.cpp)
target_link_libraries(pp-node-probe PRIVATE ${_pp_probe_libs})
pp_browser_apply_warnings(pp-node-probe)
set_target_properties(pp-node-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_pp_node_tools_out}")
install(TARGETS pp-node-probe RUNTIME DESTINATION bin)

# Stress lab load generator (docs/ops/STRESS_LAB.md): many Amp clients against one pp-node.
add_executable(pp-node-load load/main.cpp)
target_link_libraries(pp-node-load PRIVATE ${_pp_probe_libs})
pp_browser_apply_warnings(pp-node-load)
set_target_properties(pp-node-load PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_pp_node_tools_out}")
