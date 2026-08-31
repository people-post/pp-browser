# libp2p product embedding policy (product glue lives in src/base/p2p).
# A017: fork is PeerId + key wire only — no Host/TCP/Yamux/Noise tree or fork tests.

include(pp_lib_paths)

# Fixed product profile for the in-tree libp2p fork (not user options).
function(pp_lib_apply_libp2p_product_profile)
  pp_configure_status("Configuring in-tree libp2p PeerId/wire (src/lib/libp2p)...")

  # Always vendored deps from parent (Hunter removed).
  set(PACKAGE_MANAGER vendored CACHE STRING "" FORCE)
  # Embedded builds: no clang-tidy / clang-format from the fork.
  set(CLANG_TIDY OFF CACHE BOOL "" FORCE)
  set(CLANG_FORMAT OFF CACHE BOOL "" FORCE)
endfunction()
