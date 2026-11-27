# messaging — capability docs

Chat on top of delivery: its stores (as ports; the SQLite ones are the app's, CR024), inbox, groups, attachments, history, voice messages and the conversations hub.

**Paths** (under this folder; includes read as before, e.g. `"lib/messaging/…"`): [`src/lib/messaging/`](../src/lib/messaging/), [`src/lib/voice/`](../src/lib/voice/), [`src/domain/attachments/`](../src/domain/attachments/), [`src/feature/conversations/`](../src/feature/conversations/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:messaging`.

**Below it:** delivery, pricing, social and what is below them ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Architecture: [P2P_MESSAGING](../../../docs/architecture/P2P_MESSAGING.md)
- Contracts: [WIRE_SCHEMAS](../../../docs/contracts/WIRE_SCHEMAS.md), [AMP-CHANNEL § L4 mapping](../../../docs/contracts/AMP-CHANNEL.md#l4-mapping-unchanged-payloads) (its chat, history and blob channels on the mesh); the transport under them: [pp-cpp-amp's docs](https://github.com/people-post/pp-cpp-amp/blob/main/README.md#docs)
- Vectors: pp-specs `vectors/group_roster_vectors.json` (at the tag `cmake/PpPins.cmake` pins; [pp-specs](https://github.com/people-post/pp-specs/blob/main/vectors/README.md))
- Projects: [chat-storage-and-memory](../../../projects/chat-storage-and-memory/), [group-chat](../../../projects/group-chat/), [support-chat](../../../projects/support-chat/), [relay-blob-upload](../../../projects/relay-blob-upload/)
- Decisions: [CR008](../../../projects/capability-repos/DECISIONS.md#cr008--identity-is-people-the-backend-client-is-net-pricing-is-part-of-messaging), [CR020](../../../projects/capability-repos/DECISIONS.md#cr020--secure-delivery-below-messaging-and-realtime), [CR022](../../../projects/capability-repos/DECISIONS.md#cr022--capabilities-move-into-capabilitiesname-before-any-repo-split), [CR024](../../../projects/capability-repos/DECISIONS.md#cr024--capabilities-own-ideas-the-app-owns-the-database)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
