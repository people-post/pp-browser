# people — capability docs

Identity: keys, account cards, contacts, devices and the account locker's format, with the phone book's vocabulary (`common/directory`).

**Paths** (under this folder; includes read as before, e.g. `"lib/people/…"`): [`src/lib/people/`](../src/lib/people/), [`src/domain/peers/`](../src/domain/peers/), [`src/common/directory/`](../src/common/directory/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:people`.

**Below it:** the base ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Contracts: [ACCOUNT_CARD](../../../docs/contracts/ACCOUNT_CARD.md), [ACCOUNT_LOCKER](../../../docs/contracts/ACCOUNT_LOCKER.md)
- Vectors: pp-specs `vectors/`: `account_card_vectors.json`, `account_locker_vectors.json` (at the tag `cmake/PpPins.cmake` pins; [pp-specs](https://github.com/people-post/pp-specs/blob/main/vectors/README.md))
- Projects: [multi-device-account](../../../projects/multi-device-account/), [multi-relay](../../../projects/multi-relay/)
- Decisions: [CR008](../../../projects/capability-repos/DECISIONS.md#cr008--identity-is-people-the-backend-client-is-net-pricing-is-part-of-messaging), [CR013](../../../projects/capability-repos/DECISIONS.md#cr013--lib-peers-follow-the-capability-order), [CR022](../../../projects/capability-repos/DECISIONS.md#cr022--capabilities-move-into-capabilitiesname-before-any-repo-split)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
