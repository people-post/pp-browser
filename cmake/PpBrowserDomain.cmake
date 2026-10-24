include(PpBrowserWarnings)

# A domain module (src/domain/): a capability composed from lib peers, shared by features. It links
# only what it names — no pp_base, so no UI / media peers ride along unless it asks for them, and the
# headless ones are what pp-node may link (PpBrowserNoUiDeps).
function(pp_browser_add_domain_library target)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_LIBS;PRIVATE_LIBS" ${ARGN})
  add_library(${target} STATIC ${ARG_SOURCES})
  target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(${target} PUBLIC pp_pbr_common ${ARG_PUBLIC_LIBS})
  if(ARG_PRIVATE_LIBS)
    target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_LIBS})
  endif()
  pp_browser_apply_warnings(${target})
endfunction()
