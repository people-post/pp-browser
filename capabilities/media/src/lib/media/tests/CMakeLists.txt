set(_pp_media_extra_tests "")
if(APPLE)
  list(APPEND _pp_media_extra_tests ${CMAKE_CURRENT_SOURCE_DIR}/autorelease_pool_darwin_test.mm)
endif()

pp_browser_add_lib_folder_tests(
  pp_lib_media
  pp_browser_media_test
  EXTRA_SOURCES ${_pp_media_extra_tests}
  LINK_LIBS "$<$<PLATFORM_ID:Darwin>:-framework Foundation>")
