# media — capability docs

The devices, hardware codecs and media engine (`MediaEngine`) every realtime feature runs on: calls, broadcast, voice messages, the QR scanner. No control thread waits on them.

**Paths** (under this folder; includes read as before, e.g. `"lib/media/…"`): [`src/lib/media/`](../src/lib/media/), [`src/common/media_engine/`](../src/common/media_engine/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:media`.

**Below it:** the base ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Projects: [media-client-layers](../../../projects/media-client-layers/)
- Decisions: [CR023](../../../projects/capability-repos/DECISIONS.md#cr023--calls-becomes-repo-ready-first)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
