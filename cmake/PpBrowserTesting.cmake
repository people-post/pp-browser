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

# Layer CMakeLists register colocated test dirs; root adds them after libs exist.
# Base and feature modules may also add tests/ directly from their CMakeLists.
function(pp_browser_register_tests)
  foreach(subdir ${ARGN})
    set_property(GLOBAL APPEND PROPERTY PP_BROWSER_REGISTERED_TEST_DIRS
      "${CMAKE_CURRENT_SOURCE_DIR}/${subdir}")
  endforeach()
endfunction()

function(pp_browser_add_registered_tests)
  get_property(_pp_test_dirs GLOBAL PROPERTY PP_BROWSER_REGISTERED_TEST_DIRS)
  foreach(_abs ${_pp_test_dirs})
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_abs}")
    add_subdirectory("${_rel}")
  endforeach()
endfunction()

function(pp_browser_add_gtest executable_target)
  set(one_value_args WORKING_DIRECTORY)
  cmake_parse_arguments(PP_GTEST "" "${one_value_args}" "" ${ARGN})

  # A capability folder (capabilities/<name>/, CR022) labels its tests: CI skips them when nothing they use changed.
  set(_pp_labels "")
  if(PP_BROWSER_CAPABILITY)
    set(_pp_labels PROPERTIES LABELS "capability:${PP_BROWSER_CAPABILITY}")
  endif()
  if(PP_GTEST_WORKING_DIRECTORY)
    gtest_discover_tests(
      ${executable_target}
      DISCOVERY_MODE PRE_TEST
      WORKING_DIRECTORY ${PP_GTEST_WORKING_DIRECTORY}
      ${_pp_labels})
  else()
    gtest_discover_tests(
      ${executable_target}
      DISCOVERY_MODE PRE_TEST
      ${_pp_labels})
  endif()
endfunction()
