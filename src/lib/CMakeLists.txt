# Owned hard forks (lib layer). Product glue lives under src/base/{p2p,render}.
# RmlUi is provided by FetchContent / sibling pp-cpp-ui (see cmake/PpCppUi.cmake).

include(pp_lib_libp2p)

pp_lib_apply_libp2p_product_profile()
if(TESTING OR COVERAGE)
  enable_testing()
endif()
if(PP_BROWSER_LIBP2P_TESTING OR PP_BROWSER_LIBP2P_COVERAGE)
  add_subdirectory(libp2p)
else()
  add_subdirectory(libp2p EXCLUDE_FROM_ALL)
endif()
pp_configure_status("libp2p fork configured")
