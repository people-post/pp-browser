# A capability folder (capabilities/<name>/, projects/capability-repos CR022). Its CMakeLists.txt calls
# pp_browser_capability(<name>) first: the folder's src/ (which mirrors src/) becomes an include root of every
# target the lib / domain / feature helpers make under it, and its tests carry the CTest label
# capability:<name> (cmake/PpBrowserTesting.cmake), which CI skips on a PR that changes nothing they depend on.
macro(pp_browser_capability name)
  set(PP_BROWSER_CAPABILITY ${name})
  set(PP_BROWSER_CAPABILITY_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
endmacro()
