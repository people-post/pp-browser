# libp2p fork configuration and pp-browser integration glue.

function(pp_browser_define_libp2p_options)
  if(PP_BROWSER_IS_MOBILE)
    set(PP_BROWSER_LIBP2P_TESTING_DEFAULT OFF)
  else()
    set(PP_BROWSER_LIBP2P_TESTING_DEFAULT ON)
  endif()
  option(PP_BROWSER_LIBP2P_TESTING "Build in-tree libp2p unit tests" ${PP_BROWSER_LIBP2P_TESTING_DEFAULT})
  option(PP_BROWSER_LIBP2P_EXAMPLES "Build in-tree libp2p examples" OFF)
  option(PP_BROWSER_LIBP2P_COVERAGE "Enable libp2p gcovr coverage targets" OFF)
endfunction()

function(pp_browser_configure_libp2p_fork)
  pp_configure_status("Configuring in-tree libp2p (src/libp2p)...")

  set(PACKAGE_MANAGER vendored CACHE STRING "" FORCE)
  if(PP_BROWSER_IS_MOBILE)
    set(TESTING OFF CACHE BOOL "" FORCE)
    set(EXAMPLES OFF CACHE BOOL "" FORCE)
    set(COVERAGE OFF CACHE BOOL "" FORCE)
  else()
    set(TESTING ${PP_BROWSER_LIBP2P_TESTING} CACHE BOOL "Build libp2p tests" FORCE)
    set(EXAMPLES ${PP_BROWSER_LIBP2P_EXAMPLES} CACHE BOOL "Build libp2p examples" FORCE)
    set(COVERAGE ${PP_BROWSER_LIBP2P_COVERAGE} CACHE BOOL "libp2p coverage" FORCE)
  endif()
  set(CLANG_TIDY OFF CACHE BOOL "" FORCE)
  set(CLANG_FORMAT OFF CACHE BOOL "" FORCE)
endfunction()

function(pp_browser_add_libp2p_integration)
  add_library(pp_libp2p_integration STATIC
    host/Libp2pHost.cpp
    host/PeerAddressBook.cpp
    host/CircuitBridgeTarget.cpp
    host/IdentifyIntegrationService.cpp
    host/AdvertisedAddrPublisher.cpp
    host/PeerSessionManager.cpp
    host/PeerIdUtil.cpp
    host/NodeRuntime.cpp
    host/DialBackService.cpp
    host/StreamJsonFrame.cpp
    host/Reachability.cpp
    host/ReachabilityService.cpp
    host/NatTraversal.cpp
    host/CircuitRelayService.cpp
    host/MediaRelayService.cpp
  )
  target_include_directories(pp_libp2p_integration PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/libp2p/fork/include)
  target_link_libraries(pp_libp2p_integration PUBLIC
    pp_common
    p2p
    p2p_identify
    p2p_peer_id
    p2p_keys_proto
    nlohmann_json::nlohmann_json)
  if(TARGET miniupnpc-static)
    target_link_libraries(pp_libp2p_integration PRIVATE miniupnpc-static)
    target_compile_definitions(pp_libp2p_integration PUBLIC PP_BROWSER_HAS_MINIUPNPC)
    target_include_directories(pp_libp2p_integration PRIVATE
      ${CMAKE_SOURCE_DIR}/third_party/miniupnpc/include
      ${CMAKE_BINARY_DIR}/third_party/miniupnpc)
  endif()
endfunction()

function(pp_browser_add_libp2p_includes target)
  target_include_directories(${target} PUBLIC
    ${CMAKE_SOURCE_DIR}/src/libp2p/fork/include)
endfunction()
