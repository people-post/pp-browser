# delivery — capability docs

Secure delivery below messaging and realtime: pairwise and public E2E sessions, envelopes and their codecs, the control lane, the outbox, the relay's message client, and the records they carry (threads, messages, channels) with their store ports.

**Paths** (under this folder; includes read as before, e.g. `"lib/delivery/…"`): [`src/lib/delivery/`](../src/lib/delivery/), [`src/domain/delivery/`](../src/domain/delivery/), [`src/common/chat/`](../src/common/chat/), [`src/common/thread/`](../src/common/thread/). Its targets are in [`CMakeLists.txt`](../CMakeLists.txt); its tests carry the CTest label `capability:delivery`.

**Below it:** people, net and the base ([capability-repos DESIGN § Target](../../../projects/capability-repos/DESIGN.md#target), [SRC_LAYOUT § Capabilities (across layers)](../../../docs/architecture/SRC_LAYOUT.md#capabilities-across-layers)).

## Docs about it

- Contracts: [WIRE_SCHEMAS](../../../docs/contracts/WIRE_SCHEMAS.md), [MESSAGE_ENCRYPTION](../../../docs/contracts/MESSAGE_ENCRYPTION.md)
- Vectors: `src/lib/delivery/tests/data/`: `envelope_vectors.json`, `peer_request_vectors.json`, `relay_grant_vectors.json`, `relay_sign_vectors.json`
- Projects: [e2e-message-crypto](../../../projects/e2e-message-crypto/), [chat-storage-and-memory](../../../projects/chat-storage-and-memory/), [multi-relay](../../../projects/multi-relay/)
- Decisions: [CR020](../../../projects/capability-repos/DECISIONS.md#cr020--secure-delivery-below-messaging-and-realtime), [CR021](../../../projects/capability-repos/DECISIONS.md#cr021--pp-browsers-crypto-formats-are-deliverys-pp-cpp-crypto-takes-only-generic-pieces), [CR024](../../../projects/capability-repos/DECISIONS.md#cr024--capabilities-own-ideas-the-app-owns-the-database)

This folder holds the capability's own docs and leaves with its code when the capability leaves pp-browser ([CR025](../../../projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs), [CR005](../../../projects/capability-repos/DECISIONS.md#cr005--move-a-capability-only-when-a-second-app-needs-it)). The contracts and projects above stay where they are until then; shared ones go to pp-specs after release 1.
