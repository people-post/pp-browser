# libp2p hard fork

**Tier:** architecture

pp-browser vendors [cpp-libp2p](https://github.com/libp2p/cpp-libp2p) under `src/libp2p/fork/` as a **hard fork** (committed source, no git submodule).

## Layout

| Path | Role |
|------|------|
| `src/libp2p/fork/` | Upstream-shaped cpp-libp2p (`include/`, `src/`, `cmake/`, `example/`, `test/`) |
| `src/libp2p/fork/example/` | Sample programs (built when `PP_BROWSER_LIBP2P_EXAMPLES=ON`) |
| `src/libp2p/fork/test/` | Unit tests (built when `PP_BROWSER_LIBP2P_TESTING=ON` or coverage enabled) |
| `src/libp2p/fork/housekeeping/` | Coverage and local dev scripts |
| `src/libp2p/integration/host/` | `Libp2pHost` app glue (compiled into `pp_base`) |

Dependency rule inside the libp2p subtree:

```
integration/host → fork/include (public API only)
fork/src → fork/include
```

## Provenance

See [`src/libp2p/fork/UPSTREAM.json`](../../src/libp2p/fork/UPSTREAM.json) for the upstream commit SHA.

Imported from upstream commit `28e4abcea0bf3fb1b04e51febfea38305f101fe7` (2026-06-13).

## Build flags (pp-browser root CMake)

| Option | Default (desktop) | Effect |
|--------|-------------------|--------|
| `PP_BROWSER_LIBP2P_TESTING` | ON | Build `fork/test/` and link `GTest::gmock_main` |
| `PP_BROWSER_LIBP2P_EXAMPLES` | OFF | Build `fork/example/` |
| `PP_BROWSER_LIBP2P_COVERAGE` | OFF | Enable gcovr coverage targets (`ctest_coverage`, `ctest_coverage_html`) |

Mobile builds force all three OFF.

## Dependency management

Upstream uses Hunter; pp-browser **removed Hunter** and vendors all dependencies under `third_party/`:

- Import script: [`scripts/libp2p_vendor_import.sh`](../scripts/libp2p_vendor_import.sh)
- CMake wiring: [`cmake/libp2p_dependencies.cmake`](../cmake/libp2p_dependencies.cmake)
- Versions recorded in [`third_party/UPSTREAM.json`](../../third_party/UPSTREAM.json) under `libp2p_dependencies`

When `PP_BROWSER_LIBP2P_TESTING` or `PP_BROWSER_LIBP2P_COVERAGE` is ON, googletest is built from `third_party/googletest`.

## Patching policy

Edit files under `src/libp2p/fork/` directly in pp-browser commits (except `src/libp2p/integration/`, which is pp-browser-owned glue).

**pp-browser fork changes (initial import):**

- `Multihash` — inline value storage instead of `shared_ptr` (avoids null moved-from state that broke MSVC Release peer identity paths)
- `Noise` — take `IdentityManager` and copy `getKeyPair()` instead of a DI-bound `KeyPair` by value (MSVC/Boost.DI moved the same KeyPair into IdentityManager and Noise)
- `network_injector.hpp` — `bindSharedKeyPair()` returns a fresh KeyPair copy per injection from a shared store
- `host/explicit_host.*` — preferred Host factory (no Boost.DI); used by `Libp2pChatHistoryService` and `muxers_and_streams_test`. Boost.DI injectors remain for upstream-shaped examples/injector unit tests only
- `host/basic_host/basic_host.hpp` — `getIdentityManager()` for pp-browser Identify integration (L2)
- `protocol/identify/identify_push.*` — `pushUpdates()` to re-push self Identify after address-repo changes (L2)
- `CMakeLists.txt` — add `PACKAGE_MANAGER=vendored`; skip Hunter init; standalone-only cxx20 toolchain; disable install when embedded
- `cmake/dependencies.cmake` — vendored mode verifies parent-provided targets; explicit `Protobuf_INCLUDE_DIR`; GTest when testing/coverage
- `test/CMakeLists.txt` — vendored `link_libraries` for acceptance/helper test targets (qtils, gmock, secp256k1)
- `cmake/functions.cmake` — protoc via `$<TARGET_FILE:protobuf::protoc>`; fix generated `.pb.cc` paths when embedded in pp-browser
- `cmake/libp2p_add_library.cmake` — link `qtils`, `Boost::boost`, `soralog`, `Boost::Boost.DI` in vendored mode
- `cmake/install.cmake` — skip install/export when embedded in pp-browser
- `src/crypto/sha/CMakeLists.txt` — plain `target_link_libraries` signature (matches rest of tree)
- `src/security/tls/CMakeLists.txt` — link `OpenSSL::SSL` / `OpenSSL::Crypto`; include `<openssl/x509.h>` in `tls_details.cpp`
- `cmake/Hunter/` — removed (no Hunter bootstrap)

Vendored dependency patches (in `third_party/`, not the libp2p fork):

- `boringssl/CMakeLists.txt` — skip installing `bssl` on iOS (CMake requires `BUNDLE DESTINATION` for MACOSX_BUNDLE executables; app links `crypto`/`ssl` only)
- `protobuf/CMakeLists.txt` — Hunter removed
- `qtils/CMakeLists.txt`, `soralog/CMakeLists.txt` — accept `PACKAGE_MANAGER=vendored`; soralog uses `target_include_directories`
- `soralog/` — MSVC toolchain support; skip Unix-only `pthread`/`syslog` pieces on Windows/Android; `configurator_from_yaml.cpp` guards `SinkToSyslog` like Windows on Android; guard `sysexits.h` in `sink_to_file.cpp`; `util.hpp` uses generated thread names on Windows/Android; C++20 `atomic_flag` init, Clang-only sanitizer attrs, and MSVC `do/while` log macros; `level.hpp` undefs Windows `ERROR`/`DEBUG`/`IGNORE`/`min`/`max` macros before the `Level` enum and `std::min`/`std::max` (c-ares/Boost include `windows.h` first) and exposes `kLevelError`/`kLevelDebug` for call sites after `windows.h` redefines those macros; root `CMakeLists.txt` defines `NOMINMAX` for MSVC
- libp2p tests/examples — use `soralog::kLevelError` / `kLevelDebug` instead of `Level::ERROR` / `Level::DEBUG` (MSVC: `wingdi.h` `ERROR` macro)
- `boost/CMakeLists.txt` — pp-browser wrapper using Boost CMake superproject; unified `boost/` include for compiled libs
- `lsquic/` — skip duplicate `lsquic_conn_ssl.patch` on qdrvm tag; fix double-applied symbols in-tree
- lsquic — remaining patches applied at import (`cmake/patches/libp2p/lsquic/`)
- `lsquic/CMakeLists.txt` — vendored `ZLIB::ZLIB` include/link paths for Windows builds
- `soralog/` — `.github/` stripped at import (contains `aux/`, a Windows-reserved path name)

## Integration status

libp2p is built in-tree via `add_subdirectory(src/libp2p)` and linked into the `pp-browser` executable (`p2p` target). App glue lives in `src/libp2p/integration/host/`:

- `Libp2pHost.*` — shared ExplicitHost (Yamux + Noise over TCP); owned by `MessagingHub`; binds app Ed25519 identity when available
- `PeerSessionManager.*` — on-demand dial + warm-active session policy (reuse ConnectionManager; idle TTL; caps; dial backoff). Not an app-level socket pool.
- `PeerAddressBook.*` — integration-layer peer address book (media-hop **L1**): TTL’d multiaddrs per PeerId (base58); fed by bootstrap/register, inbound connections, dial success, and libp2p `AddressRepository`; exposed via `PeerSessionManager::PreferredPeerMultiaddr` for hop/circuit dial.
- `IdentifyIntegrationService.*` — wires fork **Identify** + **Identify-Push** on `BasicHost`; remote Identify refreshes L1 book; self ads via `PublishSelfAdvertisedAddrs` (media-hop **L2**).
- `BuildAdvertisedListenSet` / `AdvertisedAddrPublisher.*` — unify bound listen, UPnP external, global IPv6, and dial-back-confirmed addrs; `MessagingHub` publishes when **Node + media_relay** after reachability probe.
- `CircuitBridgeTarget.*` / `CircuitRelayService` — media-hop **L3** PeerId-friendly circuit bridge (`target_peer_id` + relay-side resolve); `PeerSessionManager::TryEnsureHopViaCircuit` for circuit-backed media-relay streams; SoftMigrate fallback via `ICircuitHopReach`.
- `PeerIdUtil.*` — derive base58 Peer ID from the app Ed25519 signing public key (network identity / Me settings; see [D096](../../projects/chat-storage-and-memory/DECISIONS.md#d096--identity-roles-peer-id-who-caip-10-find-relay-route))

Feature protocols on the shared host:

| Protocol | Service |
|----------|---------|
| `/pp-browser/chat-history/1.0.0` | `Libp2pChatHistoryService` (D060) |
| `/pp-browser/chat/1.0.0` | `Libp2pDirectChatService` (direct send/receive) |
| `/pp-browser/dial-back/1.0.0` | `DialBackService` (np seed probe; nr reachability) |
| `/pp-browser/circuit-relay/1.0.0` | `CircuitRelayService` (n3 stream bridge; not libp2p circuit v2) |
| `/pp-browser/media-relay/1.0.0` | `MediaRelayService` (n4-media blind forwarder; N021 framing/QoS) |

## TLS note

When libp2p build is enabled, curl links against vendored BoringSSL (`OpenSSL::` targets) instead of system OpenSSL on Linux.
