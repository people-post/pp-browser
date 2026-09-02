# `src/domain`

Independent **product peer** libraries (North Star). Peers must not `#include` or
`PUBLIC_LIBS`-link each other — cross-need goes through `src/common/` contracts and
`src/feature/` wiring.

| Peer (today) | Path | CMake |
|--------------|------|-------|
| people | `domain/people/` | `pp_domain_people` |
| media | `domain/media/` | `pp_domain_media` |
| net | `domain/net/` | `pp_domain_net` |
| ui | `domain/ui/` | `pp_domain_ui` (product shell: theme/catalogs/input) |
| messaging, mesh, ai | still under `src/base/` | `pp_base_*` (rename with move) |

Window host / RmlUi Backend / overlays live in `foundation/platform/ui/` (not a domain peer).

Includes: `#include "domain/people/…"`, `#include "domain/media/…"`, `#include "domain/net/…"`, `#include "domain/ui/…"`.
Old `src/base/{people,media,net,ui,render}/` stay deleted.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
