# This tree's capabilities above pp-cpp-core's five (base, product, people, net, node; projects/capability-repos
# CR027), for a build of capabilities without the app (capabilities/CMakeLists.txt, apps/node/CMakeLists.txt): what
# each builds on (DESIGN § Target; check_capability_edges.py holds this table to its own), their order, and what such
# a build adds for them. Included before pp-cpp-core's cmake/PpCapabilitiesRoot.cmake, which builds them.
set(PP_CAPABILITY_BELOW_social base people)
set(PP_CAPABILITY_BELOW_delivery base people net)
set(PP_CAPABILITY_BELOW_pricing base people net delivery)
set(PP_CAPABILITY_BELOW_media base)
set(PP_CAPABILITY_BELOW_messaging base people net social delivery pricing media)
set(PP_CAPABILITY_BELOW_calls base people net delivery pricing media)
set(PP_CAPABILITY_BELOW_broadcast base people net delivery pricing media calls)
set(PP_CAPABILITY_BELOW_relay base people net)
set(PP_CAPABILITY_BELOW_ledger base)
# After core's five, in the order a capability's targets need the ones below it defined first (src/CMakeLists.txt's).
set(PP_CAPABILITY_ORDER_EXTRA social delivery pricing media messaging calls broadcast relay ledger)
get_filename_component(PP_CAPABILITIES_DIR "${CMAKE_CURRENT_LIST_DIR}/../capabilities" ABSOLUTE)
set(PP_CAPABILITIES_APP_MODULES PpBrowserCapabilitiesBuild)
