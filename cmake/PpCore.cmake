# pp-cpp-core (projects/capability-repos CR027): base, product, people, net and node, with the cmake helpers they
# build with (Platform, the capability and test helpers, the library pins' Pp<Lib>.cmake) and their vendored curl,
# zlib and googletest. Fetched at its pin (cmake/PpPins.cmake) like any library, or from a checkout
# (-DPP_CPP_CORE_SOURCE_DIR=…, -DPP_SIBLINGS_DIR=..), and never added as a project: the app adds its five folders
# itself (src/CMakeLists.txt, or cmake/PpCapabilitiesRoot.cmake, core's, for a build without the app).
#
# Included right after project() and the pin list, before any core helper: PP_CPP_CORE_DIR is the checkout, and
# its cmake/ joins the module path after this tree's own.
include_guard(GLOBAL)

# This tree's root (its assets, third_party/, scripts/, android/), whichever root CMakeLists.txt configured it.
get_filename_component(PP_BROWSER_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

pp_pin_make_available(pp-cpp-core FILES_ONLY)
pp_pin_source_dir(pp-cpp-core PP_CPP_CORE_DIR)
list(APPEND CMAKE_MODULE_PATH "${PP_CPP_CORE_DIR}/cmake")
