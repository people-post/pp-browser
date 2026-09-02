# `src/domain`

Independent **product peer** libraries (North Star). Peers must not `#include` or
`PUBLIC_LIBS`-link each other — cross-need goes through `src/common/` contracts and
`src/feature/` wiring.

| Peer (today) | Path | CMake |
|--------------|------|-------|
| people | `domain/people/` | `pp_domain_people` |
| media | `domain/media/` | `pp_domain_media` |
| messaging, net, mesh, ai, ui, render | still under `src/base/` | `pp_base_*` (rename with move) |

Includes: `#include "domain/people/…"`, `#include "domain/media/…"`. Old `src/base/people/` and `src/base/media/` stay deleted.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
