# social — capability docs

Public posts (signed, hash-chained author logs) and the content store they share with attachments; above people, never above messaging (CR009).

**Paths** (under this folder; includes read as before, e.g. `"lib/posts/…"`): [`src/lib/posts/`](../src/lib/posts/), [`src/lib/content/`](../src/lib/content/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:social`.

**Below it:** people and the base ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Vectors: `src/lib/posts/tests/data/post_vectors.json`
- Projects: [social-posts](../../../projects/social-posts/), [content-cas](../../../projects/content-cas/)
- Decisions: [CR009](../../../projects/capability-repos/DECISIONS.md#cr009--posts-and-content-are-one-capability-below-messaging), [CR022](../../../projects/capability-repos/DECISIONS.md#cr022--capabilities-move-into-capabilitiesname-before-any-repo-split), [CR024](../../../projects/capability-repos/DECISIONS.md#cr024--capabilities-own-ideas-the-app-owns-the-database)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
