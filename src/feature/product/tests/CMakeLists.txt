# Calls and live broadcast composed with real messaging pieces (the product stack's seams).
pp_browser_add_feature_folder_tests(
  pp_feature_product
  pp_browser_feature_product_test
  LINK_LIBS pp_foundation_identity pp_foundation_diagnostics pp_mesh_testing
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
