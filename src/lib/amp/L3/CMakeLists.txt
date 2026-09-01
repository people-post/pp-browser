pp_browser_add_base_library(pp_base_mesh_channel
  SOURCES
    Capability.cpp
    ChannelBridge.cpp
    ChannelMux.cpp
    ChannelSession.cpp
    ChannelWire.cpp
    MessageReassembly.cpp
  PUBLIC_LIBS
    pp_base_mesh_session
    pp_base_adp
    pp_base_error)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
