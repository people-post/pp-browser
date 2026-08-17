# mlkem-native (PQ Code Package) — FIPS 203 ML-KEM, C backend, ML-KEM-768.
# Upstream: https://github.com/pq-code-package/mlkem-native (see UPSTREAM_COMMIT).
# RNG: MLK_CONFIG_CUSTOM_RANDOMBYTES → libsodium randombytes_buf (see mlkem_native_config.h).

cmake_minimum_required(VERSION 3.16)

add_library(mlkem_native STATIC
  mlkem/mlkem_native.c
)
add_library(mlkem_native::mlkem_native ALIAS mlkem_native)

target_include_directories(mlkem_native PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/mlkem"
)

target_link_libraries(mlkem_native PUBLIC sodium)
target_compile_features(mlkem_native PUBLIC c_std_99)

if(MSVC)
  target_compile_options(mlkem_native PRIVATE /W3)
else()
  target_compile_options(mlkem_native PRIVATE
    -Wall -Wextra -Wno-unused-function -fomit-frame-pointer
  )
endif()

set_target_properties(mlkem_native PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME mlkem_native
)
