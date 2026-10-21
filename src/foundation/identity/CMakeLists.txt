# Identity helpers for product code: the mesh PeerId derivation is pp-cpp-crypto's
# (`crypto/PeerId.h`, docs/architecture/MESH_IDENTITY.md); MultiaddrPeerId.h reads one out of a
# multiaddr. Header-only.
add_library(pp_foundation_identity INTERFACE)
target_include_directories(pp_foundation_identity INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_foundation_identity INTERFACE pp_crypto pp_pbr_common)
