# base — capability docs

The mechanisms every capability uses: `common` (the shared vocabulary, text by key, metrics and diagnostics lines, the runtime bridge) and foundation's runtime, TLS, plain TCP sockets (`foundation/sockets`: pp-node's status server and the tests' loopback HTTP servers, no Asio), errors, diagnostics, crypto and identity, on the pp-cpp-* libraries only. The product's base is not here: its headless part is `capabilities/product`, the rest the apps' own `src/foundation`.

**Paths** (under this folder; includes read as before, e.g. `"common/…"`): [`src/common/`](../src/common/), [`src/foundation/crypto/`](../src/foundation/crypto/), [`src/foundation/diagnostics/`](../src/foundation/diagnostics/), [`src/foundation/error/`](../src/foundation/error/), [`src/foundation/identity/`](../src/foundation/identity/), [`src/foundation/runtime/`](../src/foundation/runtime/), [`src/foundation/sockets/`](../src/foundation/sockets/), [`src/foundation/tls/`](../src/foundation/tls/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:base`.

**Below it:** the pp-cpp-* libraries only ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Architecture: [SRC_LAYOUT § What `capabilities/base/src/common` provides](../../../docs/architecture/SRC_LAYOUT.md#what-capabilitiesbasesrccommon-provides), [common/README](../../../capabilities/base/src/common/README.md), [THREADING](../../../docs/architecture/THREADING.md), [LOGGING](../../../docs/architecture/LOGGING.md), [MESH_IDENTITY](../../../docs/architecture/MESH_IDENTITY.md)
- Contracts: [SIGNED_EXTENSIONS](../../../docs/contracts/SIGNED_EXTENSIONS.md), [CODED_FAILURE](../../../docs/contracts/CODED_FAILURE.md), [DIAGNOSTICS](../../../docs/contracts/DIAGNOSTICS.md)
- Vectors: pp-specs `vectors/`: `alg_id_vectors.json`, `ext_area_vectors.json` (pp-cpp-crypto's and pp-cpp-common's), `kem_binding_vectors.json` (at the tag `cmake/PpPins.cmake` pins; [pp-specs](https://github.com/people-post/pp-specs/blob/main/vectors/README.md))
- Projects: [capability-repos](../../../projects/capability-repos/)
- Decisions: [CR002](../../../projects/capability-repos/DECISIONS.md#cr002--runtime-core-goes-to-pp-cpp-common), [CR015](../../../projects/capability-repos/DECISIONS.md#cr015--p2-is-gated-like-p1-the-products-base-is-the-apps), [CR021](../../../projects/capability-repos/DECISIONS.md#cr021--pp-browsers-crypto-formats-are-deliverys-pp-cpp-crypto-takes-only-generic-pieces), [CR023](../../../projects/capability-repos/DECISIONS.md#cr023--calls-becomes-repo-ready-first)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
