# Add people-post/pp-cpp-amp (ADP L1, MSH L2, channel L3, link), at its pin (cmake/PpPins.cmake; PP_CPP_AMP_GIT_TAG),
# a local checkout (PP_CPP_AMP_SOURCE_DIR) or a sibling (PP_SIBLINGS_DIR). There is no implicit sibling: a stale
# ../pp-cpp-amp must not build silently (PB-001). AMP unit/integration tests run in pp-cpp-amp CI.
#
# Public CMake target names match pp-cpp-amp: pp_amp_l1, pp_amp_l2, pp_amp_l3,
# pp_amp_link. Do not reintroduce pp_base_* aliases for these.

set(PP_AMP_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-amp tests" FORCE)

if(NOT TARGET pp_amp_l1)
  pp_pin_make_available(pp-cpp-amp)
endif()

foreach(_layer pp_amp_l1 pp_amp_l2 pp_amp_l3 pp_amp_link)
  if(NOT TARGET ${_layer})
    message(FATAL_ERROR "pp-cpp-amp did not define target ${_layer}")
  endif()
endforeach()
