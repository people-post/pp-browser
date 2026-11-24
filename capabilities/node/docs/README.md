# node — capability docs

Node services and roles: what a node hosts, its DHT and directory config, its own directory row, and the ledger role. Headless; pp-node and the product's mesh share it. pp-node itself is an app (`src/app/node/`).

**Paths** (under this folder; includes read as before, e.g. `"domain/node/…"`): [`src/domain/node/`](../src/domain/node/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:node`.

**Below it:** people, net and the base ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Architecture: [BUILD § Headless mesh node (`pp-node`)](../../../docs/ops/BUILD.md#headless-mesh-node-pp-node)
- Contracts: [NODE_METRICS](../../../docs/contracts/NODE_METRICS.md); the directory a node serves on the mesh: pp-cpp-mesh [MESH_DIRECTORY_AMP](https://github.com/people-post/pp-cpp-mesh/blob/main/docs/contracts/MESH_DIRECTORY_AMP.md)
- Projects: [p2p-mesh](../../../projects/p2p-mesh/), [hard-lab](../../../projects/hard-lab/)
- Decisions: [CR017](../../../projects/capability-repos/DECISIONS.md#cr017--pp-node-is-an-app-the-node-capability-is-domainnode), [CR022](../../../projects/capability-repos/DECISIONS.md#cr022--capabilities-move-into-capabilitiesname-before-any-repo-split)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
