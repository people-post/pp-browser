# libp2p hard fork

**Tier:** architecture

pp-browser vendors [cpp-libp2p](https://github.com/libp2p/cpp-libp2p) under `src/lib/libp2p/` as a **hard fork** (committed source, no git submodule).

## Layout

| Path | Role |
|------|------|
| `src/lib/libp2p/` | Upstream-shaped cpp-libp2p (`include/`, `src/`, `cmake/`, `example/`, `test/`) |
| `src/lib/libp2p/example/` | Sample programs (built when `PP_BROWSER_LIBP2P_EXAMPLES=ON`) |
| `src/lib/libp2p/test/` | Unit tests (built when `PP_BROWSER_LIBP2P_TESTING=ON` or coverage enabled) |
| `src/lib/libp2p/housekeeping/` | Coverage and local dev scripts |
| `src/base/p2p/` | `Libp2pHost` app glue (compiled into `pp_base`) |

Dependency rule:

```
base/p2p → lib/libp2p/include (public API only)
lib/libp2p/src → lib/libp2p/include
```

## Provenance

See [`src/lib/libp2p/UPSTREAM.json`](../../src/lib/libp2p/UPSTREAM.json) for the upstream commit SHA.

Imported from upstream commit `28e4abcea0bf3fb1b04e51febfea38305f101fe7` (2026-06-13).

## Build flags (pp-browser root CMake)

Supported knobs are `PP_BROWSER_*` only. Raw fork cache vars (`TESTING`, `EXAMPLES`, `COVERAGE`, `PACKAGE_MANAGER`) are set by the product profile in [`src/lib/pp_lib_libp2p.cmake`](../../src/lib/pp_lib_libp2p.cmake) — do not pass them on the cmake command line.

| Option | Default (desktop) | Effect |
|--------|-------------------|--------|
| `PP_BROWSER_LIBP2P_TESTING` | ON | Build `fork/test/` and link `GTest::gmock_main` |
| `PP_BROWSER_LIBP2P_EXAMPLES` | OFF | Build `fork/example/` |
| `PP_BROWSER_LIBP2P_COVERAGE` | OFF | Enable gcovr coverage targets (`ctest_coverage`, `ctest_coverage_html`) |

Mobile builds force all three OFF. Fixed profile policy: `PACKAGE_MANAGER=vendored`, clang-tidy/format off.

## Dependency management

Upstream uses Hunter; pp-browser **removed Hunter** and vendors all dependencies under `third_party/`:

- Import script: [`scripts/libp2p_vendor_import.sh`](../scripts/libp2p_vendor_import.sh)
- CMake wiring: [`cmake/libp2p_dependencies.cmake`](../cmake/libp2p_dependencies.cmake)
- Versions recorded in [`third_party/UPSTREAM.json`](../../third_party/UPSTREAM.json) under `libp2p_dependencies`

When `PP_BROWSER_LIBP2P_TESTING` or `PP_BROWSER_LIBP2P_COVERAGE` is ON, googletest is built from `third_party/googletest`.

## Patching policy

Edit files under `src/lib/libp2p/` directly in pp-browser commits (except `src/base/p2p/`, which is pp-browser-owned glue).

**pp-browser fork changes (initial import):**

- `Multihash` — inline value storage instead of `shared_ptr` (avoids null moved-from state that broke MSVC Release peer identity paths)
- `Noise` — take `IdentityManager` and copy `getKeyPair()` instead of a DI-bound `KeyPair` by value (MSVC/Boost.DI historically moved the same KeyPair into IdentityManager and Noise)
- `crypto_provider/crypto_provider_impl.hpp` — include complete `mldsa_provider.hpp` (not a forward declaration)
- `host/explicit_host.*` — sole Host factory (TCP/Quic; no Boost.DI); used by app (`Libp2pHost`), tests, and built examples; injector headers removed
- `host/basic_host/basic_host.hpp` — `getIdentityManager()` for pp-browser Identify integration (L2)
- `network/impl/listener_manager_impl.cpp` — if the host is already `start()`ed, `listen()` binds the transport immediately (needed for mobile N025 ephemeral `/tcp/0` after Client non-listen start; upstream only binds inside `start()`)
- `basic/read.hpp` / `basic/write.hpp` — return `invalid_argument` instead of throwing `std::logic_error` on zero/oversize `readSome`/`writeSome` results (uncaught throw aborted Android host io during call-media)
- `basic/write_queue.*` — enqueue copies bytes into owned `Bytes` (upstream stored `BytesIn` spans; temporaries / early buffer free → wire corruption / `too much bytes read`); soft-heal `dequeue` when `unsent` drifts from buffer layout (media_relay capture vs IO race aborted moto `assert(sz == item.unsent)`)
- `muxer/yamux/yamux_stream.*` — `stream_write_mu_` serializes stream `WriteQueue` / window / doWrite against off-strand MediaRelay `SendFrame` (capture) and connection acks/window updates
- `muxer/yamux/yamuxed_connection.*` — mutex around write queue / `is_writing_` (media-relay capture-thread `SendFrame` raced IO window-updates into `assert(!is_writing_)`)
- `basic/read_buffer.cpp` — `consumePart` soft-fails when `first_byte_offset_` is past fragment size (off-strand stream IO race aborted moto `pp-browser-io`); clear `capacity_remains_` when the last fragment is popped so `add()` does not assert `!fragments_.empty()` (Samsung media_relay dogfood SIGABRT on `pp-worker`); heal/clear on inconsistent consume; guard `consumeAll` against `first_byte_offset_` underflow (Linux PreferLocal dogfood: `free(): invalid next size`)
- `muxer/yamux/yamux_stream.cpp` — soft-fail when `consume()` returns 0 despite `size()>0` and clear buffer before arming `reading_`; `onDataReceived` drains/clears leftover buffer instead of asserting empty (moto media_relay `pp-worker` SIGABRT)
- `security/noise/noise_connection.cpp` — `readSome` with empty `out` returns 0 without pulling another Noise frame
- `protocol/identify/identify_push.*` — `pushUpdates()` to re-push self Identify after address-repo changes (L2)
- `protocol/identify/identify_delta.cpp` — create `IdentifyDeltaWire` once when sending multiple added/removed protocols (was resetting delta each loop iteration)
- **Handwritten protobuf wire** — `src/lib/libp2p/src/wire/` (`p2p_wire`): length-delimited messages encoded/decoded without `libprotobuf` or `protoc` (keys, Noise, Identify, SECIO, Plaintext, Kademlia, Gossip). `WireMessageReadWriter` replaces protobuf parse/serialize; `ProtobufMessageReadWriter` is a type alias. Vendored `third_party/protobuf` removed; `.proto` files under `*/protobuf/` remain as wire-schema docs only.
- `connection/stream_and_protocol.hpp` — forward-declare `struct Stream` (matches `stream.hpp`) so first-party `-Werror` builds do not trip `-Wmismatched-tags`
- `host/host.hpp` / `network/dialer.hpp` — `PeerInfo{.id=…, .addresses={}}` so first-party `-Werror` builds that include these headers do not trip `-Wmissing-field-initializers`
- `CMakeLists.txt` — add `PACKAGE_MANAGER=vendored`; skip Hunter init; standalone-only cxx20 toolchain; disable install when embedded
- `cmake/dependencies.cmake` — vendored mode verifies parent-provided targets; GTest when testing/coverage
- `test/CMakeLists.txt` — vendored `link_libraries` for acceptance/helper test targets (qtils, gmock)
- `cmake/libp2p_add_library.cmake` — link `qtils`, `Asio::asio`, `soralog` in vendored mode (no Boost)
- `cmake/install.cmake` — skip install/export when embedded in pp-browser
- `src/crypto/sha/CMakeLists.txt` — plain `target_link_libraries` signature (matches rest of tree)
- `src/security/tls/CMakeLists.txt` — link `OpenSSL::SSL` / `OpenSSL::Crypto`; include `<openssl/x509.h>` in `tls_details.cpp`
- `cmake/Hunter/` — removed (no Hunter bootstrap)

Vendored dependency patches (in `third_party/`, not the libp2p fork):

- `boringssl/CMakeLists.txt` — skip installing `bssl` on iOS (CMake requires `BUNDLE DESTINATION` for MACOSX_BUNDLE executables; app links `crypto`/`ssl` only)
- `qtils/CMakeLists.txt`, `soralog/CMakeLists.txt` — accept `PACKAGE_MANAGER=vendored`; soralog uses `target_include_directories`
- `qtils/outcome.hpp` — facade over vendored standalone Outcome (`third_party/outcome`, ned14 v2.2.15 single-header); keeps `outcome::result` / `OUTCOME_TRY` / `success` / `failure`; MSVC-safe `OUTCOME_TRY` via one variadic `_OUTCOME_EXPAND` (traditional MSVC preprocessor breaks `EXPAND(name)(args)`); no longer links `Boost::outcome`
- `outcome/` — standalone Outcome INTERFACE target (`Outcome::outcome`); pulled before qtils in `cmake/libp2p_dependencies.cmake`
- `libsecp256k1` / `Secp256k1Provider` — removed; `Key::Type::Secp256k1` wire code kept for marshal compatibility but crypto ops return `UNSUPPORTED_KEY_TYPE` (product identity is ML-DSA-65 / Ed25519)
- Boost + Boost.DI — removed from `third_party/` and CMake; Host wiring is `createExplicitHost` only (injector headers deleted)
- `asio/` — standalone Asio 1.34.0 (`Asio::asio`, `ASIO_STANDALONE` + `ASIO_NO_DEPRECATED`); fork, `src/base/p2p`, and `pp-node` status HTTP use `#include <asio…>` / `asio::` and `std::error_code`
- fork sources — dropped Boost.Operators (`equality_comparable`), Boost.Range (`for_each` / `filtered`), and unused Boost.Exception include in `event/bus.hpp`
- fork sources — replaced Boost.MultiIndex tables in gossip `MessageCache` and Kademlia `StorageImpl` / `ContentRoutingTableImpl` / `GetValueExecutor` with `std::unordered_map` (+ vectors); removed dead MultiIndex includes from `put_value_executor.hpp`
- fork sources — replaced Boost.Signals2 with `libp2p/event/signal.hpp` (`Signal` / `Connection` / `ScopedConnection`) for Bus, Emitter, AddressRepository, and Identify; removed unused Signals2 include from `transport_listener.hpp`
- fork sources — removed libp2p WebSocket/WSS layer (`layer/websocket`, Beast); default injectors bind no layer adaptors; multiaddr `/ws`/`/wss` codec tokens remain for address parsing only
- fork sources — TCP connect timeout uses `steady_timer` + `std::chrono` (not `deadline_timer` / `posix_time`); dropped leftover `BOOST_UNREACHABLE_RETURN` / `BOOST_NOEXCEPT` in fork sources

- `soralog/` — MSVC toolchain support; skip Unix-only `pthread`/`syslog` pieces on Windows/Android; `configurator_from_yaml.cpp` guards `SinkToSyslog` like Windows on Android; guard `sysexits.h` in `sink_to_file.cpp`; `util.hpp` uses generated thread names on Windows/Android; C++20 `atomic_flag` init, Clang-only sanitizer attrs, and MSVC `do/while` log macros; `level.hpp` undefs Windows `ERROR`/`DEBUG`/`IGNORE`/`min`/`max` macros before the `Level` enum and `std::min`/`std::max` (c-ares/Boost include `windows.h` first) and exposes `kLevelError`/`kLevelDebug` for call sites after `windows.h` redefines those macros; root `CMakeLists.txt` defines `NOMINMAX` for MSVC
- libp2p tests/examples — use `soralog::kLevelError` / `kLevelDebug` instead of `Level::ERROR` / `Level::DEBUG` (MSVC: `wingdi.h` `ERROR` macro)
- `lsquic/` — skip duplicate `lsquic_conn_ssl.patch` on qdrvm tag; fix double-applied symbols in-tree
- lsquic — remaining patches applied at import (`cmake/patches/libp2p/lsquic/`)
- `lsquic/CMakeLists.txt` — vendored `ZLIB::ZLIB` include/link paths for Windows builds
- `soralog/` — `.github/` stripped at import (contains `aux/`, a Windows-reserved path name)

## Integration status

libp2p is built in-tree via `add_subdirectory(src/lib)` and linked into the `pp-browser` executable (`p2p` target). App glue lives in `src/base/p2p/`:

- `Libp2pHost.*` — shared ExplicitHost (Yamux + Noise `/noise-mlkem768/1.0.0` over TCP); owned by `MessagingHub`; binds app device ML-DSA-65 identity when available
- `PeerSessionManager.*` — on-demand dial + warm-active session policy (reuse ConnectionManager; idle TTL; caps; dial backoff). Not an app-level socket pool.
- `PeerAddressBook.*` — integration-layer peer address book (media-hop **L1**): TTL’d multiaddrs per PeerId (base58); fed by bootstrap/register, inbound connections, dial success, and libp2p `AddressRepository`; exposed via `PeerSessionManager::PreferredPeerMultiaddr` for hop/circuit dial.
- `IdentifyIntegrationService.*` — wires fork **Identify** + **Identify-Push** on `BasicHost`; remote Identify refreshes L1 book; self ads via `PublishSelfAdvertisedAddrs` (media-hop **L2**).
- `BuildAdvertisedListenSet` / `AdvertisedAddrPublisher.*` — unify bound listen, UPnP external, global IPv6, and dial-back-confirmed addrs; `MessagingHub` publishes when **Node + media_relay** after reachability probe.
- `CircuitBridgeTarget.*` / `CircuitRelayService` — media-hop **L3** PeerId-friendly circuit bridge (`target_peer_id` + relay-side resolve); `PeerSessionManager::TryEnsureHopViaCircuit` for circuit-backed media-relay streams; SoftMigrate fallback via `ICircuitHopReach`.
- `PeerIdUtil.*` — derive base58 Peer ID from the app device ML-DSA-65 signing public key (network identity / Me settings)

### Full-PQ hard cut (libp2p-pq-transport)

| Item | Value |
|------|-------|
| Noise protocol id | `/noise-mlkem768/1.0.0` |
| Suite | `Noise_XXkem_MLKEM768_ChaChaPoly_SHA256` |
| Device identity `KeyType` | `MlDsa65 = 4` (provisional) |
| Planning | [projects/libp2p-pq-transport/](../../projects/libp2p-pq-transport/) |

Feature protocols on the shared host:

| Protocol | Service |
|----------|---------|
| `/pp-browser/chat-history/1.0.0` | `Libp2pChatHistoryService` (D060) |
| `/pp-browser/chat/1.0.0` | `Libp2pDirectChatService` (direct send/receive) |
| `/pp-browser/dial-back/1.0.0` | `DialBackService` (np seed probe; nr reachability) |
| `/pp-browser/circuit-relay/1.0.0` | `CircuitRelayService` (n3 stream bridge; not libp2p circuit v2) |
| `/pp-browser/media-relay/1.0.0` | `MediaRelayService` (n4-media blind forwarder; N021 framing/QoS) |

Stream framing (`u64-BE` length + body), protocol exchanges, and shorter/longer/hang handling: [LIBP2P_STREAMS.md](LIBP2P_STREAMS.md).

## TLS note

When libp2p build is enabled, curl links against vendored BoringSSL (`OpenSSL::` targets) instead of system OpenSSL on Linux.
