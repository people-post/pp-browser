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
  ↑
identity
```

This tree holds the foundation band: `runtime/`, `platform/`, `error/`, `i18n/`, `data/`, `crypto/`, and `identity/` (PeerId).

CMake targets: `pp_foundation_runtime(_core)`, `pp_foundation_platform(_core)`, `pp_foundation_error`, `pp_foundation_i18n`, `pp_foundation_data`, `pp_foundation_crypto`, `pp_foundation_identity`. Use `pp_browser_add_foundation_library` in this tree.

Includes: `#include "foundation/…"`. Old `src/base/{runtime,platform,error,i18n,data,crypto}` and `src/base/mesh/identity` paths stay deleted.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
