# AMP extraction readiness (pp-browser)

**Status:** Full AMP wire stack under `src/lib/amp/` (L1–L3 + link); deps limited to `pp_common` + `pp_crypto`.  
**Not started:** FetchContent wiring, deleting in-tree `src/lib/amp` after cutover.

## Layer map

| Layer | In-tree path | CMake target | Contract |
|-------|--------------|--------------|----------|
| L1 ADP wire | `src/lib/amp/L1/` | `pp_base_adp` | [ADP.md](../../docs/contracts/ADP.md) |
| L2 MSH session | `src/lib/amp/L2/` | `pp_base_mesh_session` | [AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md) |
| L3 channel mux | `src/lib/amp/L3/` | `pp_base_mesh_channel` | [AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md) |
| Link (horizontal) | `src/lib/amp/link/` | `pp_base_mesh_link` | [STACK.md](STACK.md) |
| L4 product | `src/base/p2p/`, `src/feature/messaging/` | `pp_base_p2p`, … | stays in pp-browser |

## Shared helpers (colocated in `src/lib/amp/`)

| File | Layer | Notes |
|------|-------|-------|
| `lib/amp/L1/ReplayWindow.{h,cpp}` | L1 | ADP seq window (messaging keeps separate copy in `base/crypto`) |
| `lib/amp/L1/CodedFailure.h` | L1 | `Connection::Failure` template |
| `lib/amp/L2/SessionAead.{h,cpp}` | L2 | XChaCha20-Poly1305 session AEAD |
| `lib/amp/L1/Types.h`, `lib/amp/L2/Types.h` | L1/L2 | `pp::Error` / `pp::Roe` / `ByteVector` via layer Types headers |
| `lib/amp/link/CodedFailure.h` | link | `PeerLink` / `PeerLinkManager` failures |

Product `base/crypto` helpers (`MessageCipher`, `HybridKem`, E2E codecs) stay in pp-browser; AMP uses `pp_crypto` (`MlKem`, `MlDsa`, `SodiumUtil`) directly.

## Allowed dependencies (production)

Acyclic order: `pp_common` + `pp_crypto` → L1 → L2 → L3 → link → `peer_id` → `p2p` → `feature`.

| Layer | May link | Must not link |
|-------|----------|---------------|
| `pp_base_adp` | `pp_common`, `pp_crypto`, `sodium` | `pp_base_*`, libp2p, `feature/*` |
| `pp_base_mesh_session` | L1 + `pp_crypto`, `sodium` (transitive `pp_common`) | `pp_base_*`, libp2p |
| `pp_base_mesh_channel` | L2 + L1 (transitive) | `pp_base_*`, libp2p |
| `pp_base_mesh_link` | L3 + L2 + L1 + `pp_crypto` (transitive) | `pp_base_*`, libp2p |

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
| A link | `pp_browser_amp_link_test` | `src/lib/amp/link/tests/` |
| B integration | `pp_browser_amp_integration_test` | `src/lib/amp/tests/integration/` |
| AMP test support | (compiled into Tier A link + Tier B) | `src/lib/amp/tests/support/` (PeerId stub; no `pp_base_peer_id`) |
| L4 test support | p2p/messaging compose harnesses | `src/base/p2p/tests/support/` (`mesh_harness_support` uses real `PeerIdUtil`; `mesh_triple_harness`) |

Harness headers under `lib/amp/tests/support/` are **p2p-free**. AMP-owned tests link the stub `mesh_harness_support.cpp` in that directory. L4 tests link `base/p2p/tests/support/mesh_harness_support.cpp` instead (same header, libp2p PeerId derivation).

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
