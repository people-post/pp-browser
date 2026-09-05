pp_browser_add_domain_folder_tests(
  pp_domain_ai
  pp_browser_ai_test
  PRIVATE_DEFINITIONS
    PP_BROWSER_TURN_PLAN_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/turn_plans"
  LINK_LIBS
    CURL::libcurl)
