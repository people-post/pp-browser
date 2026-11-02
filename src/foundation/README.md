# `src/foundation`

The **product's base** (CR015, [CR023](../../projects/capability-repos/DECISIONS.md) step 3): what is pp-browser's own
and the apps read — capabilities never include it.

| Folder | Target | Contents |
|--------|--------|----------|
| `product/` | `pp_foundation_product` | The product's name and slug (`ProductBranding.h`) and the build's version (`AppVersion.h`) |
| `platform/` | `pp_foundation_platform(_core)` | Platform hooks, OS integration, the window host and overlays (`platform/ui/`) |
| `i18n/` | `pp_foundation_i18n` | Text catalogs and the translator capabilities ask through (`common/Localized.h`) |
| `data/` | `pp_foundation_data` | Config, its file format, data paths |

The shared base — `common/` and foundation's runtime, TLS, errors, diagnostics, crypto and identity — is
[`capabilities/base/src/`](../../capabilities/base/src/): every capability uses it, and it uses pp-cpp-* only. Its
targets keep their names (`pp_pbr_common`, `pp_foundation_runtime_core`, `pp_foundation_crypto`, …) and paths
(`#include "foundation/crypto/…"`). The aggregate `pp_foundation` (both) is in
[`src/CMakeLists.txt`](../CMakeLists.txt).

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).

**Testing:** doctrine [TESTING.md](../../docs/architecture/TESTING.md); SQLite / temp-dir conventions
[TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md#unit-test-conventions).
