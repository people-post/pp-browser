# pricing — capability docs

Initiation pricing and payment promises: what contacting someone costs, for messages and calls alike.

**Paths** (under this folder; includes read as before, e.g. `"lib/pricing/…"`): [`src/lib/pricing/`](../src/lib/pricing/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:pricing`.

**Below it:** delivery and what is below it ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Projects: [pricing](../../../projects/pricing/)
- Decisions: [CR020](../../../projects/capability-repos/DECISIONS.md#cr020--secure-delivery-below-messaging-and-realtime), [CR022](../../../projects/capability-repos/DECISIONS.md#cr022--capabilities-move-into-capabilitiesname-before-any-repo-split)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
