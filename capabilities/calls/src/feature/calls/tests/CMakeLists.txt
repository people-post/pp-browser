# The calls feature's tests: units, and the call stack composed over loopback Amp stacks (two or more sides,
# delivery's in-memory thread store in place of messaging's SQLite one — CR023).
pp_browser_add_feature_folder_tests(pp_feature_calls pp_browser_feature_calls_test
  LINK_LIBS pp_foundation_diagnostics pp_foundation_identity pp_mesh_testing pp_lib_delivery_testing)
