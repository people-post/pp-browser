# The base's crypto tests, and the signed-format vectors they read from pp-specs at its pinned tag
# (PP_SPECS_VECTORS_DIR, cmake/PpSpecs.cmake; docs/contracts/SIGNED_EXTENSIONS.md § Vector files). pp-specs' ext_area
# and alg_id files are checked against pp-cpp-common's and pp-cpp-crypto's at their pins (the *_UPSTREAM paths: the trees
# the build used — a local checkout or sibling, else the fetched tag) while those libraries keep their own copies.
pp_pin_source_dir(pp-cpp-common _pp_common_src)
pp_pin_source_dir(pp-cpp-crypto _pp_crypto_src)

pp_browser_add_foundation_folder_tests(
  pp_foundation_crypto
  pp_browser_crypto_test
  PRIVATE_DEFINITIONS
    PP_EXT_AREA_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/ext_area_vectors.json"
    PP_ALG_ID_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/alg_id_vectors.json"
    PP_KEM_BINDING_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/kem_binding_vectors.json"
    PP_LINK_HANDSHAKE_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/link_handshake_vectors.json"
    PP_EXT_AREA_VECTORS_UPSTREAM="${_pp_common_src}/tests/data/ext_area_vectors.json"
    PP_ALG_ID_VECTORS_UPSTREAM="${_pp_crypto_src}/tests/data/alg_id_vectors.json")
