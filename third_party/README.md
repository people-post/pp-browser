# Third-party source dependencies

**The one home for vendored code** (projects/capability-repos CR012): third-party source pp-browser builds is
committed here, never fetched at configure time, and every copy is in the manifest [`UPSTREAM.json`](UPSTREAM.json) —
one entry per dependency with its `path`, `version`, `license`, upstream (`repository` + `tag` + `commit`, or
`source`) and how it is imported. A capability folder holds no vendored code; one that needs a library links its
target from here (media: Opus) or from a pinned people-post library (below).

Built via `add_subdirectory` from [`cmake/dependencies.cmake`](../cmake/dependencies.cmake). curl, zlib and
googletest are [pp-cpp-core](https://github.com/people-post/pp-cpp-core)'s (CR027): vendored there, beside the
capabilities that use them, and built from its pin (`cmake/PpCore.cmake`, its `cmake/PpCoreDependencies.cmake`).

## Here

| Directory | Upstream | Version | License | Used by |
|-----------|----------|---------|---------|---------|
| `sqlite/` | [SQLite amalgamation](https://www.sqlite.org/download.html) `3530300` | 3.53.3 | Public domain | The app's storage (`src/lib/storage`) |
| `opus/` | [xiph/opus](https://github.com/xiph/opus) `v1.5.2` | 1.5.2 | BSD-3-Clause | media (call and voice-message audio) |
| `qrcodegen/` | [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator) `v1.8.0` (C++ only) | 1.8.0 | MIT | Invite QR codes |
| `quirc/` | [dlbeer/quirc](https://github.com/dlbeer/quirc) `v1.2` (`lib/` only) | 1.2 | ISC | QR scanning (`src/lib/scan`) |

Imported by [`scripts/vendor/vendor_import.sh`](../scripts/vendor/vendor_import.sh) (SQLite, Opus, qrcodegen) and
[`quirc_import.sh`](../scripts/vendor/quirc_import.sh). The scripts refresh an
entry's upstream fields in the manifest and keep the rest; bump its `version` with the tag.

## Elsewhere in this repo

| Path | What | Why it is not here |
|------|------|--------------------|
| `android/app/src/main/java/org/libsdl/app/` | SDL 3.4.16's Java glue (Zlib), with the lines marked `pp-browser:` edited | Gradle compiles it as the app's own source; it must be the native SDL's version, which [`cmake/PpCppUi.cmake`](../cmake/PpCppUi.cmake) checks on Android. In the manifest as `sdl-android-java`. |
| `assets/fonts/` | Inter, Lato, Noto Sans CJK / SC, Noto (Color) Emoji, each beside its `*-LICENSE.txt` | Runtime assets the packages ship, not code ([BUILD.md § Dependencies](../docs/ops/BUILD.md#dependencies); `scripts/vendor/fonts_import_noto.sh`) |
| `android/gradlew`, `android/gradle/wrapper/` | The Gradle wrapper | Build tooling Gradle expects in place |

## From the pinned libraries

These are vendored in the people-post libraries, each at the tag in [`cmake/PpPins.cmake`](../cmake/PpPins.cmake), and
linked here by target; their versions and licences are in those repos. Bumping one is a library release plus a pin.

| Library | Vendored there, used directly here |
|---------|------------------------------------|
| [pp-cpp-crypto](https://github.com/people-post/pp-cpp-crypto) | libsodium (`sodium`: base, people, social, delivery, messaging, calls, broadcast, the app's storage, pp-node); BoringSSL (`crypto` / `ssl` as `OpenSSL::*`: pp-cpp-core's curl's TLS on Linux, Android and iOS, the base's TLS, the platform layer); mlkem-native, mldsa-native (through `pp_crypto`) |
| [pp-cpp-ui](https://github.com/people-post/pp-cpp-ui) | SDL3 (`PP_BROWSER_SDL3_TARGET`: media, the platform layer) and SDL3_image (the platform layer); FreeType / HarfBuzz / LunaSVG (the app and its GUI tests include `src/font/harfbuzz` from its tree); its SDL version header (the Android check above). Its libpng is built (FreeType's CBDT emoji); its zlib copy is not: FreeType takes pp-cpp-core's. |
| [pp-cpp-mesh](https://github.com/people-post/pp-cpp-mesh) | miniupnpc — not built (`PP_MESH_WITH_UPNP` is off) |
| [pp-cpp-common](https://github.com/people-post/pp-cpp-common) | JSON (`Value` / `Object`) is its own code |

**Removed:** `curl/`, `zlib/`, `googletest/` (moved to pp-cpp-core, CR027); `boringssl/` (moved to pp-cpp-crypto, which builds it for the link AEAD); `lsquic/`, `c-ares/`,
`tsl_hat_trie/` (A017: the libp2p host underlay); `yaml-cpp/`, `soralog/`, `qtils/`; `fmt/` and `outcome/` (nothing
linked them).

**Across repos** (CR012, [DESIGN § Open questions](../projects/capability-repos/DESIGN.md#open-questions) 3, CR027):
vendored code a capability uses lives with that capability's repo. curl, zlib and googletest moved to pp-cpp-core with
base, people, net and node, with their import scripts; pp-browser builds them from its pin and keeps only what its own
capabilities and app use.
