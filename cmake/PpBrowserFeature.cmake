include(PpBrowserCapability)
include(PpBrowserWarnings)

function(pp_browser_add_feature_library target)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_LIBS;PRIVATE_LIBS" ${ARGN})
  add_library(${target} STATIC ${ARG_SOURCES})
  pp_browser_own_include_root(_pp_own_root)
  target_include_directories(${target} PUBLIC ${_pp_own_root})
  if(PP_BROWSER_CAPABILITY)
    # A capability's feature module links only what it names (CR022): pp_base bundles every lib peer, the app's too.
    target_link_libraries(${target} PUBLIC pp_pbr_common ${ARG_PUBLIC_LIBS})
  else()
    target_link_libraries(${target} PUBLIC pp_base pp_pbr_common ${ARG_PUBLIC_LIBS})
  endif()
  if(ARG_PRIVATE_LIBS)
    target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_LIBS})
  endif()
  pp_browser_apply_warnings(${target})
endfunction()

function(pp_browser_add_feature_folder_tests lib_target test_target)
  cmake_parse_arguments(ARG "" "" "EXTRA_SOURCES;PRIVATE_DEFINITIONS;LINK_LIBS" ${ARGN})
  if(NOT PP_BROWSER_BUILD_TESTS)
    return()
  endif()

  file(GLOB _test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
  if(NOT _test_sources)
    return()
  endif()

  add_executable(${test_target} ${_test_sources} ${ARG_EXTRA_SOURCES})
  pp_browser_own_include_root(_pp_own_root)
  target_include_directories(${test_target} PRIVATE ${_pp_own_root})
  # A capability's tests link their module and what they name (CR022), not the app's bundles (pp_feature, pp_base).
  set(_pp_bundles pp_feature pp_base)
  if(PP_BROWSER_CAPABILITY)
    set(_pp_bundles "")
  endif()
  target_link_libraries(${test_target} PRIVATE
    GTest::gtest
    GTest::gtest_main
    ${lib_target}
    ${_pp_bundles}
    pp_pbr_common
    ${ARG_LINK_LIBS})
  if(ARG_PRIVATE_DEFINITIONS)
    target_compile_definitions(${test_target} PRIVATE ${ARG_PRIVATE_DEFINITIONS})
  endif()
  pp_browser_apply_warnings(${test_target})
  pp_browser_add_gtest(${test_target})
endfunction()
