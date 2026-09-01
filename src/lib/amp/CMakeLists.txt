# AMP stack L1–L3 (ADP wire, MSH session, channel mux). Future home: pp-cpp-amp.
include(PpBrowserBase)

add_subdirectory(L1)
add_subdirectory(L2)
add_subdirectory(L3)
add_subdirectory(link)
if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
