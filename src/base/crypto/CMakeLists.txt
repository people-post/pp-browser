pp_browser_add_base_library(pp_base_crypto
  SOURCES
    AutoKeyEstablishment.cpp
    CanonicalAad.cpp
    CryptoUtil.cpp
    DataKeyVault.cpp
    EncryptedPayload.cpp
    FileCipher.cpp
    HybridKem.cpp
    MessageCipher.cpp
    PinKeyDeriver.cpp
    PinResolver.cpp
    ProfileSecretsService.cpp
    PskBundleCodec.cpp
    PskFingerprint.cpp
    ReplayWindow.cpp
    SessionKeyDeriver.cpp
  PUBLIC_LIBS
    pp_base_error
    pp_base_data
    sodium)

if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp_base_crypto PUBLIC OpenSSL::Crypto)
  target_compile_definitions(pp_base_crypto PUBLIC OPENSSL_UNSTABLE_EXPERIMENTAL_KYBER)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
