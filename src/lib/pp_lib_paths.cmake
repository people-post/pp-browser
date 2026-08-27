# Owned hard-fork roots under src/lib/. Include early from the root CMakeLists.
# RmlUi lives in pp-cpp-ui (PP_LIB_RMLUI_ROOT / PP_LIB_RMLUI_INCLUDE set by PpCppUi.cmake).

set(PP_LIB_LIBP2P_ROOT "${CMAKE_SOURCE_DIR}/src/lib/libp2p")
set(PP_LIB_LIBP2P_INCLUDE "${PP_LIB_LIBP2P_ROOT}/include")
