pp_browser_add_base_library(pp_base_adp
  SOURCES
    Clock.cpp
    Connection.cpp
    Endpoint.cpp
    HmacBinder.cpp
    MemoryDatagramIo.cpp
    OsUdpDatagramIo.cpp
    ReplayWindow.cpp
    WireCodec.cpp
  PUBLIC_LIBS
    pp_crypto
    sodium)

if(WIN32)
  target_link_libraries(pp_base_adp PUBLIC ws2_32)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
