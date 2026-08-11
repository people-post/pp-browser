# mlkem-native (vendored)

Upstream: [pq-code-package/mlkem-native](https://github.com/pq-code-package/mlkem-native) tag **v2.0.0** (commit in `UPSTREAM_COMMIT`).

pp-browser uses the **C backend**, single parameter set **ML-KEM-768**, namespace prefix `mlkem`, monolithic `mlkem/mlkem_native.c`.

RNG: `MLK_CONFIG_CUSTOM_RANDOMBYTES` → libsodium `randombytes_buf` (patched into `mlkem_native_config.h` by `vendor_import.sh`). CMake target `mlkem_native` links `sodium`.

Do not edit sources for app features — wrap in `src/base/crypto/HybridKem`. Re-import via `scripts/vendor_import.sh` when bumping.
