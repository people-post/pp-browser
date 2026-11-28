# ledger — capability docs

pp-node's ledger role (pp-ledger platform-integration pi4): one pp-ledger relay or miner on the node's mesh runtime, with the node's identity and address, from a work directory of its own. A node service (node-relay NR001): both pp-node presets link it; the desktop app never does. Built only with pp-ledger (`PP_BROWSER_WITH_LEDGER`; not on mobile), which it links privately. It runs only when asked (`--ledger-role`).

**Paths** (under this folder; includes read as before the move, `"domain/ledger/…"`): [`src/domain/ledger/`](../src/domain/ledger/). Its code is in `pbr::ledger`; its library defines `PP_NODE_HAS_LEDGER`. Its tests carry the CTest label `capability:ledger`.

**Below it:** the base. It goes with the pp-node repo ([CR027](../../../projects/capability-repos/DECISIONS.md#cr027--layered-repos-each-born-on-demand-instead-of-one-capabilities-repo)): pp-node is its only user.

## Docs about it

- Ops: [BUILD § Headless mesh node](../../../docs/ops/BUILD.md#headless-mesh-node-pp-node) (the ledger role), [MESH](../../../docs/architecture/MESH.md)
- Projects: pp-ledger's platform-integration (pi4)
