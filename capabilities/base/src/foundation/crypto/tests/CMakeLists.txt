# The base's crypto tests, and the signed-format vectors they read (docs/contracts/SIGNED_EXTENSIONS.md § Vector files):
# copies of pp-cpp-common's ext_area_vectors.json and pp-cpp-crypto's alg_id_vectors.json, checked against the pinned
# tags' files (the *_UPSTREAM paths) and read through pp-browser's helpers.
if(PP_CPP_COMMON_SOURCE_DIR AND EXISTS "${PP_CPP_COMMON_SOURCE_DIR}/CMakeLists.txt")
  set(_pp_common_src "${PP_CPP_COMMON_SOURCE_DIR}")
else()
  FetchContent_GetProperties(pp_cpp_common SOURCE_DIR _pp_common_src)
endif()
FetchContent_GetProperties(pp_cpp_crypto SOURCE_DIR _pp_crypto_src)

pp_browser_add_foundation_folder_tests(
  pp_foundation_crypto
  pp_browser_crypto_test
  PRIVATE_DEFINITIONS
    PP_EXT_AREA_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/ext_area_vectors.json"
    PP_ALG_ID_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/alg_id_vectors.json"
    PP_KEM_BINDING_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/kem_binding_vectors.json"
    PP_EXT_AREA_VECTORS_UPSTREAM="${_pp_common_src}/tests/data/ext_area_vectors.json"
    PP_ALG_ID_VECTORS_UPSTREAM="${_pp_crypto_src}/tests/data/alg_id_vectors.json")
