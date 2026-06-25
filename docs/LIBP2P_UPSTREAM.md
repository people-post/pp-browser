# libp2p hard fork

pp-browser vendors [cpp-libp2p](https://github.com/libp2p/cpp-libp2p) under `src/libp2p/` as a **hard fork** (committed source, no git submodule).

## Provenance

See [`src/libp2p/UPSTREAM.json`](../src/libp2p/UPSTREAM.json) for the upstream commit SHA.

Imported from upstream commit `28e4abcea0bf3fb1b04e51febfea38305f101fe7` (2026-06-13).

## Dependency management

Upstream uses Hunter; pp-browser **removed Hunter** and vendors all dependencies under `third_party/`:

- Import script: [`scripts/libp2p_vendor_import.sh`](../scripts/libp2p_vendor_import.sh)
- CMake wiring: [`cmake/libp2p_dependencies.cmake`](../cmake/libp2p_dependencies.cmake)
- Versions recorded in [`third_party/UPSTREAM.json`](../third_party/UPSTREAM.json) under `libp2p_dependencies`

## Patching policy

Edit files under `src/libp2p/` directly in pp-browser commits (except `src/libp2p/integration/`, which is pp-browser-owned glue).

**pp-browser fork changes (initial import):**

- `CMakeLists.txt` — add `PACKAGE_MANAGER=vendored`; skip Hunter init; standalone-only cxx20 toolchain; disable install when embedded
- `cmake/dependencies.cmake` — vendored mode verifies parent-provided targets; explicit `Protobuf_INCLUDE_DIR`
- `cmake/functions.cmake` — protoc via `$<TARGET_FILE:protobuf::protoc>`; fix generated `.pb.cc` paths when embedded in pp-browser
- `cmake/libp2p_add_library.cmake` — link `qtils`, `Boost::boost`, `soralog`, `Boost::Boost.DI` in vendored mode
- `cmake/install.cmake` — skip install/export when embedded in pp-browser
- `src/crypto/sha/CMakeLists.txt` — plain `target_link_libraries` signature (matches rest of tree)
- `src/security/tls/CMakeLists.txt` — link `OpenSSL::SSL` / `OpenSSL::Crypto`; include `<openssl/x509.h>` in `tls_details.cpp`
- `cmake/Hunter/` — removed (no Hunter bootstrap)

Vendored dependency patches (in `third_party/`, not the libp2p fork):

- `protobuf/CMakeLists.txt` — Hunter removed
- `qtils/CMakeLists.txt`, `soralog/CMakeLists.txt` — accept `PACKAGE_MANAGER=vendored`; soralog uses `target_include_directories`
- `soralog/` — MSVC toolchain support; skip Unix-only `pthread`/`syslog` pieces on Windows; guard `sysexits.h` in `sink_to_file.cpp`; `util.hpp` uses generated thread names on Windows/Android; C++20 `atomic_flag` init, Clang-only sanitizer attrs, and MSVC `do/while` log macros; `level.hpp` undefs Windows `ERROR`/`DEBUG`/`IGNORE` macros before the `Level` enum (c-ares includes `windows.h` first)
- `boost/CMakeLists.txt` — pp-browser wrapper using Boost CMake superproject; unified `boost/` include for compiled libs
- `lsquic/` — skip duplicate `lsquic_conn_ssl.patch` on qdrvm tag; fix double-applied symbols in-tree
- lsquic — remaining vcpkg-overlay patches applied at import (`cmake/patches/libp2p/lsquic/`)
- `lsquic/CMakeLists.txt` — vendored `ZLIB::ZLIB` include/link paths for Windows builds
- `soralog/` — `.github/` stripped at import (contains `aux/`, a Windows-reserved path name)

## Integration status

libp2p is built in-tree via `add_subdirectory(src/libp2p)` but **not yet linked** into the `pp-browser` executable. Future app glue belongs in `src/libp2p/integration/`.

## TLS note

When libp2p build is enabled, curl links against vendored BoringSSL (`OpenSSL::` targets) instead of system OpenSSL on Linux.
