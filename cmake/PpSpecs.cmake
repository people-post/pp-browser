# pp-specs (projects/capability-repos CR025): the contracts more than one repository implements, and their frozen
# vectors, which every implementation's tests read at its pinned tag instead of keeping a copy. Files only: nothing
# to build, fetched only when the tests are (cmake/PpPins.cmake has the tag; -DPP_SPECS_SOURCE_DIR=… or sibling mode
# reads a local checkout). Sets PP_SPECS_VECTORS_DIR, the folder the tests' PP_<NAME>_VECTORS_PATH name.

if(NOT PP_BROWSER_BUILD_TESTS)
  return()
endif()

pp_pin_make_available(pp-specs FILES_ONLY)
pp_pin_source_dir(pp-specs _pp_specs_src)
set(PP_SPECS_VECTORS_DIR "${_pp_specs_src}/vectors")
if(NOT EXISTS "${PP_SPECS_VECTORS_DIR}/manifest.json")
  message(FATAL_ERROR "pp-specs at ${_pp_specs_src} has no vectors/manifest.json: not a pp-specs checkout?")
endif()
