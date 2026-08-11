# mldsa-native (PQ Code Package) — FIPS 204 ML-DSA, C backend, ML-DSA-65.
# Upstream: https://github.com/pq-code-package/mldsa-native (see UPSTREAM_COMMIT).
# RNG: MLD_CONFIG_CUSTOM_RANDOMBYTES → libsodium randombytes_buf (see mldsa_native_config.h).

cmake_minimum_required(VERSION 3.16)

add_library(mldsa_native STATIC
  mldsa/mldsa_native.c
)
add_library(mldsa_native::mldsa_native ALIAS mldsa_native)

target_include_directories(mldsa_native PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/mldsa"
)

target_link_libraries(mldsa_native PUBLIC sodium)
target_compile_features(mldsa_native PUBLIC c_std_99)

if(MSVC)
  target_compile_options(mldsa_native PRIVATE /W3)
else()
  target_compile_options(mldsa_native PRIVATE
    -Wall -Wextra -Wno-unused-function -fomit-frame-pointer
  )
endif()

set_target_properties(mldsa_native PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME mldsa_native
)
