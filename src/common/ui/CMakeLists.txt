include(PpBrowserWarnings)

# The shell's working-set types and their codec (app side, common layer): the AI's planner and the GUI share them.
add_library(pp_common_ui STATIC WorkingSetCodec.cpp)
target_include_directories(pp_common_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_common_ui PUBLIC pp_pbr_common)
pp_browser_apply_warnings(pp_common_ui)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
