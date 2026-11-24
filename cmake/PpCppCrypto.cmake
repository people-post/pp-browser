# Add people-post/pp-cpp-crypto (sodium + ML-KEM/ML-DSA + BoringSSL + pp_crypto), at its pin (cmake/PpPins.cmake;
# PP_CPP_CRYPTO_GIT_TAG, a release tag on that repo's main line), a local checkout (PP_CPP_CRYPTO_SOURCE_DIR) or a
# sibling (PP_SIBLINGS_DIR). Never a checkout it was not told about.

set(PP_CRYPTO_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-crypto tests" FORCE)

pp_pin_make_available(pp-cpp-crypto)

if(NOT TARGET pp_crypto)
  message(FATAL_ERROR "pp-cpp-crypto did not define target pp_crypto")
endif()
if(NOT TARGET sodium)
  message(FATAL_ERROR "pp-cpp-crypto did not define target sodium")
endif()
if(NOT TARGET mldsa_native OR NOT TARGET mlkem_native)
  message(FATAL_ERROR "pp-cpp-crypto did not define mldsa_native / mlkem_native")
endif()
