# Fetch people-post/pp-cpp-crypto (sodium + ML-KEM/ML-DSA + pp_crypto).
#
# Pin a release tag from that repo's main line (PP_CPP_CRYPTO_GIT_TAG).
# Do not track develop/main branch tips or auto-pick a sibling checkout.

include(FetchContent)

set(PP_CPP_CRYPTO_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-crypto.git"
  CACHE STRING "Git remote for pp-cpp-crypto")
set(PP_CPP_CRYPTO_GIT_TAG "v0.2.0"
  CACHE STRING "Release tag on pp-cpp-crypto main (not a branch name)")

set(PP_CRYPTO_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-crypto tests" FORCE)

FetchContent_Declare(
  pp_cpp_crypto
  GIT_REPOSITORY ${PP_CPP_CRYPTO_GIT_REPOSITORY}
  GIT_TAG ${PP_CPP_CRYPTO_GIT_TAG}
)
FetchContent_MakeAvailable(pp_cpp_crypto)

if(NOT TARGET pp_crypto)
  message(FATAL_ERROR "pp-cpp-crypto did not define target pp_crypto")
endif()
if(NOT TARGET sodium)
  message(FATAL_ERROR "pp-cpp-crypto did not define target sodium")
endif()
if(NOT TARGET mldsa_native OR NOT TARGET mlkem_native)
  message(FATAL_ERROR "pp-cpp-crypto did not define mldsa_native / mlkem_native")
endif()
