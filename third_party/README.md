# Third-party source dependencies

Vendored upstream libraries, kept in the repository and built via `add_subdirectory` from
[`cmake/dependencies.cmake`](../cmake/dependencies.cmake) and
[`cmake/libp2p_dependencies.cmake`](../cmake/libp2p_dependencies.cmake) (named after the retired
libp2p fork, A017).

Elsewhere: JSON (`Value` / `Object`) in [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common);
libsodium, mlkem-native and mldsa-native in [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto);
RmlUi, FreeType / HarfBuzz / LunaSVG and SDL3 / SDL3_image in
[`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui); the mesh in
[`pp-cpp-mesh`](https://github.com/people-post/pp-cpp-mesh) on [`pp-cpp-amp`](https://github.com/people-post/pp-cpp-amp).

## App libraries

Imported by [`scripts/vendor/vendor_import.sh`](../scripts/vendor/vendor_import.sh); exact commits in
[`UPSTREAM.json`](UPSTREAM.json).

| Directory | Upstream | Tag | License | Used by |
|-----------|----------|-----|---------|---------|
| `curl/` | [curl/curl](https://github.com/curl/curl) | `curl-8_11_1` | curl license | HTTP (LLM, Brief, pp-node directory publish) |
| `sqlite/` | [SQLite amalgamation](https://www.sqlite.org/download.html) | `3.53.3` (`3530300`) | Public domain | Thread store, sessions |
| `opus/` | [xiph/opus](https://github.com/xiph/opus) | `v1.5.2` | BSD | Call audio |

## Shared libraries

Imported by [`scripts/vendor/libp2p_vendor_import.sh`](../scripts/vendor/libp2p_vendor_import.sh), but
Asio (a standalone tree).

| Directory | Upstream | Used by |
|-----------|----------|---------|
| `boringssl/` | [qdrvm/boringssl](https://github.com/qdrvm/boringssl) `qdrvm1` | curl's TLS on Linux, Android and iOS (Windows: Schannel; macOS: SecureTransport). Nothing else |
| `zlib/` | qdrvm/zlib 1.3.0-p1 | curl; FreeType reuses it |
| `asio/` | [chriskohlhoff/asio](https://github.com/chriskohlhoff/asio) 1.34.0 | pp-node's status HTTP server; test HTTP servers |
| `googletest/` | google/googletest v1.14.0 | Unit tests (`PP_BROWSER_BUILD_TESTS`) |

**Removed:** `lsquic/`, `c-ares/`, `tsl_hat_trie/` (A017: the libp2p host underlay); `yaml-cpp/`,
`soralog/`, `qtils/`; `fmt/` and `outcome/` (nothing linked them).
