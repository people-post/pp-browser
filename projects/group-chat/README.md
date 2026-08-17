# Group chat

**Status:** In progress — Bucket C  
**Owner:** Hongwei + agents  
**Stable refs:** [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md), [WIRE_SCHEMAS.md](../../docs/contracts/WIRE_SCHEMAS.md), [chat-storage D076/D089/D095](../chat-storage-and-memory/DECISIONS.md), [e2e E022](../e2e-message-crypto/DECISIONS.md)

## One-line goal

Multi-party E2E group chat with owner-signed membership, invite/block controls, and fork-with-members — built on pairwise sender-keys (N ciphertexts per message) and relaxed ingest (D046).

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Membership protocol, roles, invite policy, fork semantics, wire schema |
| [DECISIONS.md](DECISIONS.md) | ADRs (G001+) |

## Dependencies

| Prerequisite | Status |
|--------------|--------|
| Private direct E2E (waves 1–7) | Done |
| `e2e_public` auto-key (E013/E024) | Required for pairwise key bootstrap |
| Group wire + N ciphertexts (D095) | This project |

## Phases

| Phase | Scope |
|-------|--------|
| C0 | `e2e_public` auto-key send/compose enablement |
| C1 | `route.kind=group`, N-ciphertext send/receive |
| C2 | Membership events (invite/accept/remove/leave/fork) |
| C3 | Roster storage, group-scoped sync |
| C4 | UI — create group, roster, invite cards, block settings |
| C5 | Fork-with-members (fresh start v1) |
