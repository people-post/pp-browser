pp_browser_add_lib_folder_tests(
  pp_lib_media
  pp_browser_media_test
  PRIVATE_DEFINITIONS
    PP_BROWSER_MEDIA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  LINK_LIBS qrcodegen)
