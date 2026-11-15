pp_browser_add_lib_folder_tests(
  pp_lib_media
  pp_browser_media_test)

# MediaEngine's temporary lifecycle.probe lines read AppLifecycle (capabilities/base, GUI builds only).
if(TARGET pp_foundation_runtime)
  target_link_libraries(pp_browser_media_test PRIVATE pp_foundation_runtime)
endif()
