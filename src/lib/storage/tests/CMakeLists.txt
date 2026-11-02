# The SQLite stores' own tests: schema, migrations, what SQLite does that the ports promise; and the contract tests
# that run each case on a SQLite store and on the in-memory one the capabilities' tests use.
pp_browser_add_lib_folder_tests(
  pp_lib_storage
  pp_browser_storage_test
  LINK_LIBS pp_lib_delivery_testing pp_lib_messaging_testing pp_lib_calls_testing)
