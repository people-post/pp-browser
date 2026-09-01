# AMP extraction readiness (pp-browser)

**Status:** L1–L3 colocated under `src/lib/amp/` on `dev-team-1` for future `pp-cpp-amp` extraction.  
**Not started:** FetchContent wiring, deleting in-tree `src/lib/amp` after cutover.

## Layer map

| Layer | In-tree path | CMake target | Contract |
|-------|--------------|--------------|----------|
| L1 ADP wire | `src/lib/amp/L1/` | `pp_base_adp` | [ADP.md](../../docs/contracts/ADP.md) |
| L2 MSH session | `src/lib/amp/L2/` | `pp_base_mesh_session` | [AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md) |
| L3 channel mux | `src/lib/amp/L3/` | `pp_base_mesh_channel` | [AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md) |
| Link (horizontal) | `src/base/mesh/link/` | `pp_base_mesh_link` | [STACK.md](STACK.md) |
| L4 product | `src/base/p2p/`, `src/feature/messaging/` | `pp_base_p2p`, … | stays in pp-browser |

## Allowed dependencies (production)

Acyclic order: `crypto` → `adp` → `mesh/session` → `mesh/channel` → `mesh/link` → `peer_id` → `p2p` → `feature`.

| Layer | May link | Must not link |
|-------|----------|---------------|
| `pp_base_adp` | `pp_base_crypto`, `pp_base_error`, `sodium` | `p2p`, libp2p, `feature/*` |
| `pp_base_mesh_session` | `pp_base_adp`, `pp_base_crypto`, `sodium` | `p2p`, libp2p |
| `pp_base_mesh_channel` | session + adp (transitive) | `p2p`, libp2p |
| `pp_base_mesh_link` | channel + session + adp + crypto | `p2p`, libp2p |

`PeerLinkConfig::peer_id_from_identity` injects PeerId derivation at runtime — link libraries do not call libp2p directly ([A025](DECISIONS.md#a025--pre-extract-layer-cleanup-limits-policies-peerid)).

## Shared helpers (move with L1/L2 on extract)

| File | Used by | Notes |
|------|---------|-------|
| `src/base/crypto/ReplayWindow.{h,cpp}` | L1 (`Connection`) | L1-adjacent; optional colocation under `adp/` |
| `src/base/error/CodedFailure.h` | L1 (`Connection::Failure`) | Generic coded-error template |
| `src/base/crypto/HybridKem.{h,cpp}` | L2 MSH | Session handshake KEM |
| `src/base/crypto/MlDsa.{h,cpp}` | L2 MSH, tests | Identity bind |
| `src/base/crypto/MessageCipher.{h,cpp}` | L2 Session AEAD | XChaCha20-Poly1305 |
| `src/base/crypto/CryptoUtil.{h,cpp}` | L2 keys / wire | Hex, sodium init |
| `src/base/crypto/CryptoConstants.h` | L2 | Wire constants |
| `src/base/crypto/CryptoTypes.h` | L2 | `ByteVector`, key types |
| `src/common/PbrCompat.h` | All AMP layers | `pbr::` aliases for `pp::` types |

## Stays in pp-browser

- `pp_base_peer_id` — libp2p PeerId encode (`PeerIdUtil`)
- L4 coordinators, product channel policies (`ProductChannelPolicies.h`)
- `mesh_triple_harness` consumers (circuit compose tests in `pp_base_p2p`)
- GUI, SQLite, libp2p Host glue

## Test layout (after prep)

| Tier | Target | Location |
|------|--------|----------|
| A L1 | `pp_browser_adp_test` | `src/lib/amp/L1/tests/` |
| A L2 | `pp_browser_amp_session_test` | `src/lib/amp/L2/tests/` |
| A L3 | `pp_browser_amp_channel_test` | `src/lib/amp/L3/tests/` |
| A link | `pp_browser_amp_link_test` | `src/base/mesh/link/tests/` |
| B integration | `pp_browser_amp_integration_test` | `src/base/mesh/tests/integration/` |
| Support | (compiled into targets above) | `src/base/mesh/tests/support/` |

Harness headers are **p2p-free**; only `mesh_harness_support.cpp` links `pp_base_peer_id`.

Matrices: [L1_TEST_MATRIX.md](L1_TEST_MATRIX.md), [L2_TEST_MATRIX.md](L2_TEST_MATRIX.md), [L3_TEST_MATRIX.md](L3_TEST_MATRIX.md), [LINK_TEST_MATRIX.md](LINK_TEST_MATRIX.md), [TEST_MATRIX.md](TEST_MATRIX.md) (Tier B).

## Do not do yet

1. Add `cmake/PpCppAmp.cmake` or FetchContent `pp-cpp-amp`
2. Delete in-tree `src/lib/amp` after pp-cpp-amp cutover
3. Wire pp-ledger to shared AMP
4. Replace real `PeerIdUtil` in product paths with test stubs

## Future cutover checklist (one PR after `pp-cpp-amp` lands)

1. `include(PpCppAmp)` in root `CMakeLists.txt` (after `PpCppCrypto`)
2. Remove `add_subdirectory(amp)` from `src/lib/CMakeLists.txt`
3. Keep legacy aliases (`pp_base_adp` → fetched lib) or thin forwarding headers
4. Run all five AMP ctest targets + harness-dependent p2p/messaging tests
