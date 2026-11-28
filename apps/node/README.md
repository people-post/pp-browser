# apps/node — pp-node

The headless node, an app of its own (capability-repos CR017, [CR028](../../projects/capability-repos/DECISIONS.md#cr028--pp-node-becomes-repo-ready-in-place)): its two builds, `pp-node` and `pp-node-plus` (node-relay NR001), their tests and the node's lab tools (`pp-node-probe`, `pp-node-load`). Its include root is `src/` here (`"app/node/…"`), never pp-browser's `src/`: it reaches the rest only through the capabilities it links (base, product, people, net, node, relay) and the pinned libraries.

- **Build:** pp-browser's configure adds this folder (binaries under `build/src/app/node/`), or on its own: `cmake -S apps/node -B build-node` ([BUILD § Headless mesh node](../../docs/ops/BUILD.md#headless-mesh-node-pp-node)).
- **Config:** its own (`NodeConfig`, `NodeConfigLoad`), read from the same file keys with the app's rules ([CONFIGURATION § pp-node deploy overlays](../../docs/ops/CONFIGURATION.md#pp-node-deploy-overlays)).
- **Packaging and release:** `packaging/pp-node/`, `scripts/platform/pp_node_package_linux.sh`, `.github/workflows/release-pp-node.yml`.

With the add-on capabilities, this folder is what the pp-node repo takes (CR027).
