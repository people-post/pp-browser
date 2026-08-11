# mldsa-native (vendored)

Upstream: [pq-code-package/mldsa-native](https://github.com/pq-code-package/mldsa-native) tag **v2.0.0** (commit in `UPSTREAM_COMMIT`).

pp-browser uses the **C backend**, single parameter set **ML-DSA-65** (account signing), namespace prefix `mldsa`, monolithic `mldsa/mldsa_native.c`.

RNG: `MLD_CONFIG_CUSTOM_RANDOMBYTES` → libsodium `randombytes_buf` (patched into `mldsa_native_config.h` by `vendor_import.sh`). CMake target `mldsa_native` links `sodium`.

Do not edit sources for app features — wrap in `src/base/crypto/MlDsa`. Re-import via `scripts/vendor_import.sh` when bumping.
