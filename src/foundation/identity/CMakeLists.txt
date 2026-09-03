pp_browser_add_foundation_library(pp_foundation_identity
  SOURCES
    MlDsaPublicKeyWire.cpp
    Multihash.cpp
    Base58.cpp
    PeerId.cpp
    PeerIdUtil.cpp
  PUBLIC_LIBS
    pp_foundation_crypto
    pp_common)

if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp_foundation_identity PUBLIC OpenSSL::Crypto)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
