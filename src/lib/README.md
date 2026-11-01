# `src/lib`

**Product peer** libraries (North Star). A peer may `#include` and `PUBLIC_LIBS`-link a peer of a
**lower capability** (`lib/messaging` → `lib/people`; [capability-repos CR013](../../projects/capability-repos/DECISIONS.md#cr013--lib-peers-follow-the-capability-order));
two peers of one capability, or an edge against the order, may not — that cross-need goes through
`src/common/` contracts and `src/feature/` wiring.

| Peer (today) | Path | CMake |
|--------------|------|-------|
| people | [`capabilities/people/src/lib/people/`](../../capabilities/people/src/lib/people/) (CR022) | `pp_lib_people` |
| media | [`capabilities/calls/src/lib/media/`](../../capabilities/calls/src/lib/media/) (CR022) | `pp_lib_media` |
| net | [`capabilities/net/src/lib/net/`](../../capabilities/net/src/lib/net/) (CR022) | `pp_lib_net` |
| ui | `lib/ui/` | `pp_lib_ui` (product shell: theme/catalogs/input) |
| delivery | [`capabilities/delivery/src/lib/delivery/`](../../capabilities/delivery/src/lib/delivery/) (CR020, CR022) | `pp_lib_delivery` |
| messaging | [`capabilities/messaging/src/lib/messaging/`](../../capabilities/messaging/src/lib/messaging/) (CR022) | `pp_lib_messaging` |
| calls | [`capabilities/calls/src/lib/calls/`](../../capabilities/calls/src/lib/calls/) (CR022) | `pp_lib_calls` |
| broadcast | `lib/broadcast/` | `pp_lib_broadcast` |
| pricing | [`capabilities/pricing/src/lib/pricing/`](../../capabilities/pricing/src/lib/pricing/) (CR022) | `pp_lib_pricing` |
| content | [`capabilities/social/src/lib/content/`](../../capabilities/social/src/lib/content/) (CR022) | `pp_lib_content` |
| posts | [`capabilities/social/src/lib/posts/`](../../capabilities/social/src/lib/posts/) (CR022) | `pp_lib_posts` |
| ai | `lib/ai/` | `pp_lib_ai` (+ conversation/mcp sublibs) |

The mesh is not a peer here: it is [pp-cpp-mesh](https://github.com/people-post/pp-cpp-mesh)
(`#include "mesh/…"`, `pp_mesh`), which `feature/` and `app/` compose. Domain code may embed only its
vocabulary (`mesh/types/…`, `pp_mesh_types`, through `pp_pbr_common`) — `check_base_includes.sh`.

Window host / RmlUi Backend / overlays live in `foundation/platform/ui/` (not a lib peer).

Includes: `#include "lib/people/…"`, `#include "lib/media/…"`, `#include "lib/net/…"`, `#include "lib/ui/…"`, `#include "lib/messaging/…"`, `#include "lib/calls/…"`, `#include "lib/broadcast/…"`, `#include "lib/pricing/…"`, `#include "lib/content/…"`, `#include "lib/posts/…"`, `#include "lib/ai/…"`.
Aggregate `pp_lib` is defined in this folder’s [`CMakeLists.txt`](CMakeLists.txt). Product-stack convenience `pp_base` (foundation + Amp + `pp_lib`) is in [`src/CMakeLists.txt`](../CMakeLists.txt).

**Testing:** lib peers should be almost fully coverable by **unit** tests (and loopback compose where the peer owns the stack). Prefer extracting a seam over sprawling integration when a path is too hard. Doctrine: [TESTING.md](../../docs/architecture/TESTING.md). Add `tests/README.md` under a peer only when coverage/skips are non-obvious.

North Star: [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md).
