# Mesh identity (PeerId)

**Status:** Native implementation in `src/base/mesh/identity/` (libp2p fork deleted).

## Scope

Derives libp2p-compatible base58 PeerIds from ML-DSA-65 device public keys (1952 bytes). Wire format is unchanged:

- Multiaddr segment: `/p2p/<PeerId>`
- JSON call control field: `libp2p_peer_id` (name retained for compat)

## Derivation pipeline

1. Protobuf-encode `PublicKeyWire` with `KeyTypeWire::kMlDsa65 = 4` (fields: type, data).
2. If encoded key ≤ 42 bytes → identity multihash; else SHA-256 multihash of encoded bytes.
3. Base58-encode multihash buffer → PeerId string (`Qm…` for ML-DSA keys).

ML-DSA-65 keys always take the SHA-256 path (`Qm…` prefix).

## Public API

| Header | Symbol |
|--------|--------|
| `base/mesh/identity/PeerIdUtil.h` | `PeerIdFromMlDsaPublicKey()` |

CMake target: `pp_base_mesh_identity` (linked by `pp_base_mesh` and `pp_domain_people`).

## Golden vectors

Fixed tests in `src/base/mesh/tests/peer_id_util_test.cpp` gate byte-compat before any identity change:

| Key fixture | Expected PeerId |
|-------------|-----------------|
| 1952 × `0x42` | `QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP` |
| 1952 × `0x01` | `QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie` |

## MeshHost wiring

`MeshHost::StartAmpFromConfig` sets `peer_id_from_identity` on the Amp link config so product PeerIds match `PeerIdFromMlDsaPublicKey`.

## History

The vendored cpp-libp2p fork (`src/lib/libp2p/`) retained only PeerId + key wire (A017). That fork was absorbed into `base/mesh/identity` and removed; see [MESH.md](MESH.md).
