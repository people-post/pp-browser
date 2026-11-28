# Plain TCP sockets (capability-repos CR027): listen, accept, connect to loopback, send, receive, poll. pp-node's status
# server and the tests' loopback HTTP servers use them, so the base needs no Asio. POSIX or Winsock by OS.
if(WIN32)
  set(_pp_sockets_os TcpSockets_Win32.cpp)
else()
  set(_pp_sockets_os TcpSockets_Posix.cpp)
endif()

pp_browser_add_foundation_library(pp_foundation_sockets
  SOURCES
    TcpSockets.cpp
    ${_pp_sockets_os})
if(WIN32)
  target_link_libraries(pp_foundation_sockets PUBLIC ws2_32)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
