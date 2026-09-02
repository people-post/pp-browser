include(PpBrowserBase)

# Foundation bands (ordered): runtime + platform → error/i18n → data → crypto → identity.
add_subdirectory(runtime)
add_subdirectory(platform)
add_subdirectory(i18n)
add_subdirectory(error)
add_subdirectory(data)
add_subdirectory(crypto)
add_subdirectory(identity)

# Convenience INTERFACE: all foundation modules (not Amp — Amp is FetchContent lib).
add_library(pp_foundation INTERFACE)
target_link_libraries(pp_foundation INTERFACE
  pp_foundation_error
  pp_foundation_runtime_core
  pp_foundation_runtime
  pp_foundation_platform_core
  pp_foundation_platform
  pp_foundation_data
  pp_foundation_i18n
  pp_foundation_crypto
  pp_foundation_identity
)
