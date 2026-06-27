include(GoogleTest)

function(pp_browser_enable_googletest)
  if(TARGET GTest::gtest)
    return()
  endif()

  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(gmock_force_shared_crt ON CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_build_tests OFF CACHE BOOL "" FORCE)
  set(gtest_build_samples OFF CACHE BOOL "" FORCE)

  add_subdirectory(
    "${CMAKE_SOURCE_DIR}/third_party/googletest"
    "${CMAKE_BINARY_DIR}/third_party/googletest"
    EXCLUDE_FROM_ALL)
endfunction()

function(pp_browser_add_test test_name executable_target)
  add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${executable_target}>)
  if(ARGN)
    set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY ${ARGN})
  endif()
endfunction()

function(pp_browser_add_gtest executable_target)
  set(one_value_args WORKING_DIRECTORY)
  cmake_parse_arguments(PP_GTEST "" "${one_value_args}" "" ${ARGN})

  if(PP_GTEST_WORKING_DIRECTORY)
    gtest_discover_tests(
      ${executable_target}
      DISCOVERY_MODE PRE_TEST
      WORKING_DIRECTORY ${PP_GTEST_WORKING_DIRECTORY})
  else()
    gtest_discover_tests(
      ${executable_target}
      DISCOVERY_MODE PRE_TEST)
  endif()
endfunction()
