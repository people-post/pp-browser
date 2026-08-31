# libp2p product embedding policy (product glue lives in src/base/p2p).
# A017: fork is PeerId + key wire only — no Host/TCP/Yamux/Noise tree or fork tests.

include(pp_lib_paths)

function(pp_browser_define_libp2p_options)
  # Retained for config compatibility; Host underlay + fork tests were deleted (A017).
  option(PP_BROWSER_LIBP2P_TESTING "Deprecated (A017): libp2p Host tests removed" OFF)
  option(PP_BROWSER_LIBP2P_EXAMPLES "Deprecated (A017): libp2p examples removed" OFF)
  option(PP_BROWSER_LIBP2P_COVERAGE "Deprecated (A017): libp2p coverage removed" OFF)
endfunction()

# Fixed product profile for the in-tree libp2p fork (not user options).
function(pp_lib_apply_libp2p_product_profile)
  pp_configure_status("Configuring in-tree libp2p PeerId/wire (src/lib/libp2p)...")

  # Always vendored deps from parent (Hunter removed).
  set(PACKAGE_MANAGER vendored CACHE STRING "" FORCE)
  # Embedded builds: no clang-tidy / clang-format from the fork.
  set(CLANG_TIDY OFF CACHE BOOL "" FORCE)
  set(CLANG_FORMAT OFF CACHE BOOL "" FORCE)
  set(TESTING OFF CACHE BOOL "" FORCE)
  set(EXAMPLES OFF CACHE BOOL "" FORCE)
  set(COVERAGE OFF CACHE BOOL "" FORCE)
endfunction()
