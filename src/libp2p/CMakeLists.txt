# pp-browser libp2p subtree: upstream-shaped cpp-libp2p fork + pp-browser integration glue.
include(PpBrowserLibp2p)

pp_browser_configure_libp2p_fork()

if(TESTING OR COVERAGE)
  enable_testing()
endif()
if(PP_BROWSER_LIBP2P_TESTING OR PP_BROWSER_LIBP2P_COVERAGE)
  add_subdirectory(fork)
else()
  add_subdirectory(fork EXCLUDE_FROM_ALL)
endif()

add_subdirectory(integration)
pp_configure_status("libp2p configured")
