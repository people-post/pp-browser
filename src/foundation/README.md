# `src/foundation`

Shared **kernel** implementations every domain peer may use. Ordered bands (not fully peer-independent).

```
runtime, platform(_core)
  ↑
error, i18n
  ↑
data
  ↑
crypto
```

Today this tree holds `runtime/`, `platform/`, `error/`, `i18n/`, and `data/`. Remaining foundation module still under [`src/base/`](../base/): `crypto/` (next peel).

CMake targets keep transitional `pp_base_runtime*` / `pp_base_platform*` / `pp_base_error` / `pp_base_i18n` / `pp_base_data` names until the broader `pp_foundation_*` rename.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
