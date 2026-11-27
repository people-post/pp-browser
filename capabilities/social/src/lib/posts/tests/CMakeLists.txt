pp_browser_add_lib_folder_tests(
  pp_lib_posts
  pp_browser_posts_test
  LINK_LIBS pp_lib_posts_testing
  PRIVATE_DEFINITIONS
    PP_POST_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/post_vectors.json")
