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
| messaging | `domain/messaging/` | `pp_domain_messaging` |
| ai | `domain/ai/` | `pp_domain_ai` (+ conversation/mcp sublibs) |
| mesh | `domain/mesh/` | `pp_domain_mesh` |

Window host / RmlUi Backend / overlays live in `foundation/platform/ui/` (not a domain peer).

Includes: `#include "domain/people/…"`, `#include "domain/media/…"`, `#include "domain/net/…"`, `#include "domain/ui/…"`, `#include "domain/messaging/…"`, `#include "domain/ai/…"`, `#include "domain/mesh/…"`.
Aggregate `pp_domain` is defined in this folder’s [`CMakeLists.txt`](CMakeLists.txt). Product-stack convenience `pp_base` (foundation + Amp + `pp_domain`) is in [`src/CMakeLists.txt`](../CMakeLists.txt).

**Testing:** domain peers should be almost fully coverable by **unit** tests (and loopback compose where the peer owns the stack). Prefer extracting a seam over sprawling integration when a path is too hard. Doctrine: [TESTING.md](../../docs/architecture/TESTING.md). Add `tests/README.md` under a peer only when coverage/skips are non-obvious.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
