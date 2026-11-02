# The calls feature's tests: units, and the call stack composed over loopback Amp stacks (two or more sides,
# delivery's and calls' in-memory stores in place of the app's SQLite ones — CR023, CR024).
pp_browser_add_feature_folder_tests(pp_feature_calls pp_browser_feature_calls_test
  LINK_LIBS pp_foundation_diagnostics pp_foundation_identity pp_mesh_testing pp_lib_delivery_testing pp_lib_calls_testing)
