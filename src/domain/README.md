# `src/domain`

Independent **product peer** libraries (North Star). Peers must not `#include` or
`PUBLIC_LIBS`-link each other — cross-need goes through `src/common/` contracts and
`src/feature/` wiring.

| Peer (today) | Path | CMake |
|--------------|------|-------|
| people | `domain/people/` | `pp_domain_people` |
| messaging, net, mesh, media, ai, ui, render | still under `src/base/` | `pp_base_*` (rename with move) |

Includes: `#include "domain/people/…"`. Old `src/base/people/` stays deleted.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
