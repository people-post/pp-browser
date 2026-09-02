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

Today this tree holds `runtime/`, `platform/`, `error/`, and `i18n/`. Remaining foundation modules still live under [`src/base/`](../base/) until follow-up moves (`data/`, `crypto/`).

CMake targets keep transitional `pp_base_runtime*` / `pp_base_platform*` / `pp_base_error` / `pp_base_i18n` names until the broader `pp_foundation_*` rename.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
