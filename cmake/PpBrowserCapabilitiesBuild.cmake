# What a build of capabilities without the app adds for this tree's own (cmake/PpBrowserCapabilities.cmake), once
# pp-cpp-core's cmake/PpCapabilitiesRoot.cmake knows which it builds (PP_CAPABILITIES_BUILT): pp-ledger for ledger,
# this tree's vendored code (SQLite for relay, Opus for media), pp-cpp-ui's SDL3 for media's devices.
if("ledger" IN_LIST PP_CAPABILITIES_BUILT)
  include(PpLedger)
endif()
include(dependencies)
if(NOT PP_BROWSER_HEADLESS AND "media" IN_LIST PP_CAPABILITIES_BUILT)
  include(PpCppUi) # SDL3 for media's devices
endif()
