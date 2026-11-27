# relay — capability docs

The message relay, the first add-on of `pp-node-plus` (node-relay NR001): a relay of the relay protocol on the node's mesh. The mailbox comes in NR3; the relay protocol's lockers, cards and directory follow, and from NR6 it wakes phones through the org's push gateway. Its files are in `<profile dir>/relay/`. Only `pp-node-plus` links add-ons. The default `pp-node` never does, and its build checks that. Headless; its code is in namespace `pbr::relay`, and its library defines `PP_NODE_HAS_RELAY`. Both binaries are built from `src/app/node/`. It has its settings and its store (`RelaySettings`, `RelayStore`; off until `node.relay.enabled`); NR3's second half adds the protocol.

**Paths** (under this folder; includes read as before, e.g. `"domain/relay/…"`): [`src/domain/relay/`](../src/domain/relay/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:relay`.

**Below it:** people, net and the base (beside node, not on it) ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Architecture: [BUILD § Headless mesh node (`pp-node`)](../../../docs/ops/BUILD.md#headless-mesh-node-pp-node)
- Contracts: the relay protocol, pp-specs `contracts/relay/` (draft: [pp-specs #4](https://github.com/people-post/pp-specs/pull/4))
- Projects: [node-relay](../../../projects/node-relay/)
- Decisions: [NR001](../../../projects/node-relay/DECISIONS.md#nr001--two-tiers-pp-node-and-pp-node-plus-one-build-for-every-add-on)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)).
