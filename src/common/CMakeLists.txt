include(PpBrowserWarnings)

# pp-browser cross-module utilities (FetchContent pp_common is the external common layer).
add_library(pp_pbr_common STATIC
  CallMediaHealth.cpp
  DirectoryJson.cpp
  LengthPrefixedCodec.cpp
)
target_include_directories(pp_pbr_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_pbr_common PUBLIC pp_common)
pp_browser_apply_warnings(pp_pbr_common)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
