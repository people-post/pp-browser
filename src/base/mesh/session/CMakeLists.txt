pp_browser_add_base_library(pp_base_mesh_session
  SOURCES
    MshHandshake.cpp
    MshMessages.cpp
    Session.cpp
    SessionControl.cpp
    SessionCrypto.cpp
    SessionKeys.cpp
  PUBLIC_LIBS
    pp_base_adp
    pp_base_crypto
    pp_base_error
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
