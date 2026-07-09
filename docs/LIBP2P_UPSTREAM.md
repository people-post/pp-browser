# libp2p hard fork

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

See [`src/libp2p/fork/UPSTREAM.json`](../src/libp2p/fork/UPSTREAM.json) for the upstream commit SHA.

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
- Versions recorded in [`third_party/UPSTREAM.json`](../third_party/UPSTREAM.json) under `libp2p_dependencies`

When `PP_BROWSER_LIBP2P_TESTING` or `PP_BROWSER_LIBP2P_COVERAGE` is ON, googletest is built from `third_party/googletest`.

## Patching policy

Edit files under `src/libp2p/fork/` directly in pp-browser commits (except `src/libp2p/integration/`, which is pp-browser-owned glue).

**pp-browser fork changes (initial import):**

- `Noise` — take `IdentityManager` and copy `getKeyPair()` instead of a DI-bound `KeyPair` by value (MSVC/Boost.DI moved the same KeyPair into IdentityManager and Noise)
- `network_injector.hpp` — `bindSharedKeyPair()` returns a fresh KeyPair copy per injection from a shared store
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

- `protobuf/CMakeLists.txt` — Hunter removed
- `qtils/CMakeLists.txt`, `soralog/CMakeLists.txt` — accept `PACKAGE_MANAGER=vendored`; soralog uses `target_include_directories`
- `soralog/` — MSVC toolchain support; skip Unix-only `pthread`/`syslog` pieces on Windows/Android; `configurator_from_yaml.cpp` guards `SinkToSyslog` like Windows on Android; guard `sysexits.h` in `sink_to_file.cpp`; `util.hpp` uses generated thread names on Windows/Android; C++20 `atomic_flag` init, Clang-only sanitizer attrs, and MSVC `do/while` log macros; `level.hpp` undefs Windows `ERROR`/`DEBUG`/`IGNORE`/`min`/`max` macros before the `Level` enum and `std::min`/`std::max` (c-ares/Boost include `windows.h` first); root `CMakeLists.txt` defines `NOMINMAX` for MSVC
- `boost/CMakeLists.txt` — pp-browser wrapper using Boost CMake superproject; unified `boost/` include for compiled libs
- `lsquic/` — skip duplicate `lsquic_conn_ssl.patch` on qdrvm tag; fix double-applied symbols in-tree
- lsquic — remaining patches applied at import (`cmake/patches/libp2p/lsquic/`)
- `lsquic/CMakeLists.txt` — vendored `ZLIB::ZLIB` include/link paths for Windows builds
- `soralog/` — `.github/` stripped at import (contains `aux/`, a Windows-reserved path name)

## Integration status

libp2p is built in-tree via `add_subdirectory(src/libp2p)` and linked into the `pp-browser` executable (`p2p` target). App glue is a stub in `src/libp2p/integration/host/Libp2pHost.*`; transport wiring lands separately.

## TLS note

When libp2p build is enabled, curl links against vendored BoringSSL (`OpenSSL::` targets) instead of system OpenSSL on Linux.
