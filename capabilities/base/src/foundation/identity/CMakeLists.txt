# The mesh PeerId for product code: the derivation is pp-cpp-crypto's (`crypto/PeerId.h`,
# docs/architecture/MESH_IDENTITY.md); reading one out of a multiaddr is common/MultiaddrPeerId.h. Link-only.
add_library(pp_foundation_identity INTERFACE)
target_include_directories(pp_foundation_identity INTERFACE ${PP_BROWSER_CAPABILITY_SRC})
target_link_libraries(pp_foundation_identity INTERFACE pp_crypto pp_pbr_common)
