# relay — capability docs

The relay services `pp-node-relay` runs beside a node's own (node-relay NR001): the mailbox (NR3), later the push gateway and the `files` service (NR6). Only `pp-node-relay` links it. The default `pp-node` never does, and its build checks that. Headless. Both binaries are built from `src/app/node/`. None of the services exists yet (NR2).

**Paths** (under this folder; includes read as before, e.g. `"domain/relay/…"`): [`src/domain/relay/`](../src/domain/relay/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:relay`.

**Below it:** node, people, net and the base ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Architecture: [BUILD § Headless mesh node (`pp-node`)](../../../docs/ops/BUILD.md#headless-mesh-node-pp-node)
- Contracts: the relay protocol, pp-specs `contracts/relay/` (draft: [pp-specs #4](https://github.com/people-post/pp-specs/pull/4))
- Projects: [node-relay](../../../projects/node-relay/)
- Decisions: [NR001](../../../projects/node-relay/DECISIONS.md#nr001--pp-node-relay-a-second-build-of-pp-node-not-a-plugin-or-a-repo)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)).
