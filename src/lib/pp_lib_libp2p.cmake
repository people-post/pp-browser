# libp2p product embedding policy (product glue lives in src/base/p2p).
# User-facing knobs: PP_BROWSER_LIBP2P_TESTING / EXAMPLES / COVERAGE.

include(pp_lib_paths)

function(pp_browser_define_libp2p_options)
  if(PP_BROWSER_IS_MOBILE)
    set(PP_BROWSER_LIBP2P_TESTING_DEFAULT OFF)
  else()
    # A017: Host/Yamux/Noise tests are not product path; keep Opt-in until tree delete.
    set(PP_BROWSER_LIBP2P_TESTING_DEFAULT OFF)
  endif()
  option(PP_BROWSER_LIBP2P_TESTING "Build in-tree libp2p unit tests" ${PP_BROWSER_LIBP2P_TESTING_DEFAULT})
  option(PP_BROWSER_LIBP2P_EXAMPLES "Build in-tree libp2p examples" OFF)
  option(PP_BROWSER_LIBP2P_COVERAGE "Enable libp2p gcovr coverage targets" OFF)
endfunction()

# Fixed product profile for the in-tree libp2p fork (not user options).
function(pp_lib_apply_libp2p_product_profile)
  pp_configure_status("Configuring in-tree libp2p (src/lib/libp2p)...")

  # Always vendored deps from parent (Hunter removed).
  set(PACKAGE_MANAGER vendored CACHE STRING "" FORCE)
  # Embedded builds: no clang-tidy / clang-format from the fork.
  set(CLANG_TIDY OFF CACHE BOOL "" FORCE)
  set(CLANG_FORMAT OFF CACHE BOOL "" FORCE)

  if(PP_BROWSER_IS_MOBILE)
    set(TESTING OFF CACHE BOOL "" FORCE)
    set(EXAMPLES OFF CACHE BOOL "" FORCE)
    set(COVERAGE OFF CACHE BOOL "" FORCE)
  else()
    set(TESTING ${PP_BROWSER_LIBP2P_TESTING} CACHE BOOL "Build libp2p tests" FORCE)
    set(EXAMPLES ${PP_BROWSER_LIBP2P_EXAMPLES} CACHE BOOL "Build libp2p examples" FORCE)
    set(COVERAGE ${PP_BROWSER_LIBP2P_COVERAGE} CACHE BOOL "libp2p coverage" FORCE)
  endif()
endfunction()
