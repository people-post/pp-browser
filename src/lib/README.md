# `src/lib`

Independent **product peer** libraries (North Star). Peers must not `#include` or
`PUBLIC_LIBS`-link each other — cross-need goes through `src/common/` contracts and
`src/feature/` wiring.

| Peer (today) | Path | CMake |
|--------------|------|-------|
| people | `lib/people/` | `pp_lib_people` |
| media | `lib/media/` | `pp_lib_media` |
| net | `lib/net/` | `pp_lib_net` |
| ui | `lib/ui/` | `pp_lib_ui` (product shell: theme/catalogs/input) |
| messaging | `lib/messaging/` | `pp_lib_messaging` |
| calls | `lib/calls/` | `pp_lib_calls` |
| broadcast | `lib/broadcast/` | `pp_lib_broadcast` |
| pricing | `lib/pricing/` | `pp_lib_pricing` |
| content | `lib/content/` | `pp_lib_content` |
| ai | `lib/ai/` | `pp_lib_ai` (+ conversation/mcp sublibs) |

The mesh is not a peer here: it is [pp-cpp-mesh](https://github.com/people-post/pp-cpp-mesh)
(`#include "mesh/…"`, `pp_mesh`), which `feature/` and `app/` compose. Domain code may embed only its
vocabulary (`mesh/types/…`, `pp_mesh_types`, through `pp_pbr_common`) — `check_base_includes.sh`.

Window host / RmlUi Backend / overlays live in `foundation/platform/ui/` (not a lib peer).

Includes: `#include "lib/people/…"`, `#include "lib/media/…"`, `#include "lib/net/…"`, `#include "lib/ui/…"`, `#include "lib/messaging/…"`, `#include "lib/calls/…"`, `#include "lib/broadcast/…"`, `#include "lib/pricing/…"`, `#include "lib/content/…"`, `#include "lib/ai/…"`.
Aggregate `pp_lib` is defined in this folder’s [`CMakeLists.txt`](CMakeLists.txt). Product-stack convenience `pp_base` (foundation + Amp + `pp_lib`) is in [`src/CMakeLists.txt`](../CMakeLists.txt).

**Testing:** lib peers should be almost fully coverable by **unit** tests (and loopback compose where the peer owns the stack). Prefer extracting a seam over sprawling integration when a path is too hard. Doctrine: [TESTING.md](../../docs/architecture/TESTING.md). Add `tests/README.md` under a peer only when coverage/skips are non-obvious.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
