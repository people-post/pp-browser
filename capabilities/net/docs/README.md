# net — capability docs

The Brief backend client: registration, the directory, blobs, push and the relay's certificate. Deployment-specific product code (CR008).

**Paths** (under this folder; includes read as before, e.g. `"lib/net/…"`): [`src/lib/net/`](../src/lib/net/), [`src/domain/registration/`](../src/domain/registration/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:net`.

**Below it:** people and the base ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Contracts: [SERVICE_ENDPOINTS](../../../docs/contracts/SERVICE_ENDPOINTS.md)
- Vectors: pp-specs `vectors/relay_certificate_vectors.json` (at the tag `cmake/PpPins.cmake` pins; [pp-specs](https://github.com/people-post/pp-specs/blob/main/vectors/README.md))
- Projects: [push-notifications](../../../projects/push-notifications/), [relay-blob-upload](../../../projects/relay-blob-upload/), [multi-relay](../../../projects/multi-relay/), [capability-repos § A second app is waiting](../../../projects/capability-repos/CURRENT_STATE.md#a-second-app-is-waiting-2026-11-22)
- Decisions: [CR008](../../../projects/capability-repos/DECISIONS.md#cr008--identity-is-people-the-backend-client-is-net-pricing-is-part-of-messaging), [CR022](../../../projects/capability-repos/DECISIONS.md#cr022--capabilities-move-into-capabilitiesname-before-any-repo-split)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
