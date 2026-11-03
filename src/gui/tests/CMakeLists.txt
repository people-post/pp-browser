pp_browser_add_feature_folder_tests(
  pp_gui
  pp_browser_gui_test
  LINK_LIBS pp_lib_net_testing
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
    PP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

include(pp_lib_rmlui)
pp_browser_link_rmlui_core(pp_browser_gui_test)

# font_weights_test.cpp uses the HarfBuzz font engine the app uses (and looks at its glyphs).
target_include_directories(pp_browser_gui_test PRIVATE ${PP_LIB_UI_ROOT}/src ${PP_LIB_UI_ROOT}/src/font/harfbuzz)
