pp_browser_add_feature_folder_tests(
  pp_feature_conversations
  pp_browser_feature_conversations_test
  LINK_LIBS pp_foundation_identity pp_mesh_testing pp_domain_net_testing
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
