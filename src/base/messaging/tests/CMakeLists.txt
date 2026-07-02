add_executable(pp_browser_sqlite_thread_store_test sqlite_thread_store_test.cpp)
target_include_directories(pp_browser_sqlite_thread_store_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_sqlite_thread_store_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_sqlite_thread_store_test)
