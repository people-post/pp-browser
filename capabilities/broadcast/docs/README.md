# broadcast — capability docs

Live broadcast: join tickets, viewer ladders, relay media, the RPC codec, the signed peer-announce feed and the viewer workflow. A sibling of calls on the same neutral layers; calls never names it.

**Paths** (under this folder; includes read as before, e.g. `"lib/broadcast/…"`): [`src/lib/broadcast/`](../src/lib/broadcast/), [`src/feature/broadcast/`](../src/feature/broadcast/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:broadcast`.

**Below it:** calls and what is below it ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Vectors: pp-specs `vectors/`: `broadcast_join_ticket_vectors.json`, `peer_announce_tip_vectors.json` (at the tag `cmake/PpPins.cmake` pins; [pp-specs](https://github.com/people-post/pp-specs/blob/main/vectors/README.md))
- Projects: [peer-scoped-broadcast](../../../projects/peer-scoped-broadcast/), [media-client-layers](../../../projects/media-client-layers/)
- Decisions: [CR003](../../../projects/capability-repos/DECISIONS.md#cr003--calls-and-broadcast-are-one-repo-realtime), [CR022](../../../projects/capability-repos/DECISIONS.md#cr022--capabilities-move-into-capabilitiesname-before-any-repo-split)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
