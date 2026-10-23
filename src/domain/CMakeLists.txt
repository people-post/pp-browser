include(PpBrowserDomain)

# Domain modules: capabilities composed from lib peers that features share (docs/architecture/
# SRC_LAYOUT.md). Acyclic, in this order: a module may use only the ones above it
# (scripts/check/check_domain_includes.sh). Each names what it links (no pp_base).
add_subdirectory(registration)
add_subdirectory(node)
add_subdirectory(call_media)

set(_pp_domain_modules
  pp_domain_registration
  pp_domain_node
  pp_domain_call_media
)

# Aggregate for this build's domain modules (mirrors pp_lib).
add_library(pp_domain INTERFACE)
target_link_libraries(pp_domain INTERFACE ${_pp_domain_modules})
unset(_pp_domain_modules)
