# product — capability docs

The headless part of the product's base that both apps use identically (capability-repos [CR028](../../../projects/capability-repos/DECISIONS.md#cr028--pp-node-becomes-repo-ready-in-place)): pp-browser and pp-node read the same backends, keep their files in the same layout, open the same profiles and name their builds the same way. Only apps include it (CR015). It builds on the base alone, and it goes to pp-cpp-core ([CR027](../../../projects/capability-repos/DECISIONS.md#cr027--layered-repos-each-born-on-demand-instead-of-one-capabilities-repo)).

| Folder | Target | Contents |
|--------|--------|----------|
| [`src/foundation/product/`](../src/foundation/product/) | `pp_foundation_product` | The build's identity: its version (`AppVersion.h`), the commit and tag (`BuildInfo.h`), the dev-tool switches (`DevTools.h`) |
| [`src/foundation/platform/`](../src/foundation/platform/) | `pp_product_platform` | The backends a build can run and what is compiled in for them: origins, relay ids, relay certificates, the tester gate, `relay.content_sha256` (`DeploymentProfile.h`, `BackendTable.h`); the platform (`Platform.h`), its paths (`IPathProvider.h`, `DesktopPathProvider.h`), the log defaults; the executable's path, the data-folder lock, process stats, thread names (`os/`) |
| [`src/foundation/data/`](../src/foundation/data/) | `pp_product_data` | Where the apps keep their files (`AppPaths.h`), the profiles and their manifests, the mesh role, and the config file's sections both apps read (`ConfigSectionsJson.h`: mesh, service endpoints, a custom backend's services) |

**Not here** (the desktop app's, in [`src/foundation/`](../../../src/foundation/README.md)): its config (`AppConfig`, `Config`, `ConfigJson`), preferences and session, its text catalogs, its name and slug (`ProductBranding.h`), and the platform hooks only the app uses (assets, credentials, the GUI platform). pp-node's config is its own (`src/app/node/NodeConfig.h`), read from the same file keys with the same rules.

**Paths:** includes read as before (`"foundation/platform/DeploymentProfile.h"`). Its tests carry the CTest label `capability:product`; the platform ones run headless now, as pp-node's build does.

**Below it:** the base.

## Docs about it

- Contracts: [DATA_LAYOUT](../../../docs/contracts/DATA_LAYOUT.md) (the data root, profiles); [SERVICE_ENDPOINTS](../../../docs/contracts/SERVICE_ENDPOINTS.md) (backends and their relays)
- Ops: [CONFIGURATION](../../../docs/ops/CONFIGURATION.md), [BUILD § Headless mesh node](../../../docs/ops/BUILD.md#headless-mesh-node-pp-node)
- Projects: [profile-model](../../../projects/profile-model/), [release-compatibility](../../../projects/release-compatibility/) (the tester gate, RC022), [capability-repos](../../../projects/capability-repos/)

This folder holds the capability's own docs and leaves with its code ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)).
