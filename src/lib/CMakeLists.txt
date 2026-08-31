# Owned hard forks (lib layer). Product glue lives under src/base/{p2p,render}.
# RmlUi is provided by FetchContent / sibling pp-cpp-ui (see cmake/PpCppUi.cmake).

include(pp_lib_libp2p)

pp_lib_apply_libp2p_product_profile()
# PeerId/wire targets are linked by product; EXCLUDE_FROM_ALL avoids orphan install rules.
add_subdirectory(libp2p EXCLUDE_FROM_ALL)
pp_configure_status("libp2p fork configured")
