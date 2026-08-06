# P2P A/V calls

**Status:** **a4 thin landed**; **V026 / m1** = libp2p-only voice (Android↔Android LAN dogfood OK 2026-08-02); **m2** = WebRTC teardown next  
**Owner:** Hongwei + agents  
**Stable refs:** (promote after ship) wire / wake / media-key contracts  
**Related:** [NETWORKING.md](../../docs/architecture/NETWORKING.md), [p2p-mesh](../p2p-mesh/) (N022), [media-hop-reachability](../media-hop-reachability/), [group-chat](../group-chat/), [e2e-message-crypto](../e2e-message-crypto/), [push-notifications](../push-notifications/), [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md), [CALLS.md](../../docs/architecture/CALLS.md)

## One-line goal

1:1 and group **voice** (video deferred) over **libp2p** — HTTP for backends when available; mesh signaling + blind `media_relay`; app E2E call media key.

## Release scope (v1)

| In | Out |
|----|-----|
| 1:1 + group **voice** (mobile + desktop) on **libp2p** | Call recording; **video deferred** (V026) |
| Separate `call_id` session (≠ chat thread) | Screen share |
| Invite to join (guests + late join); any group member may start | Ambient “Join” for all group members without invite |
| Hostless (ends when last participant leaves) | End-for-all host control |
| Shared call media key; rotate on leave (overlapping epochs) | Per-user media ciphertexts (chat N-key model) |
| `call_wake` push for ringing | Rich media in push payload |
| Call started / ended system hints in origin chat history | Full call transcript / recording archive |
| Soft max **16** participants (engineering may cap **8** until SFU proven) | PSTN / dial-in; WebRTC/ICE product path |

## Documents

| File | Tier | Purpose |
|------|------|---------|
| [../../docs/architecture/CALLS.md](../../docs/architecture/CALLS.md) | **Stable** (architecture) | Code map — planes, modules, topology ownership, extraction target |
| [DESIGN.md](DESIGN.md) | **Active** (project) | Product design — entities, lifecycle, crypto, invite rules |
| [DECISIONS.md](DECISIONS.md) | **Active** (project) | ADRs (V001+) — rationale; promote wire shapes to `docs/contracts/` when shipped |
| [CURRENT_STATE.md](CURRENT_STATE.md) | **Active** (project) | Dogfood / codebase board this week |
| [PHASES.md](PHASES.md) | **Active** (project) | Delivery checklist (v0 → a6) |
| [HOST_RECEIVE_POLICY.md](HOST_RECEIVE_POLICY.md) | **Active** (project) | Host admit / queue / drop / meter matrix (V032) |

Agents: code “where does X live?” → **CALLS**. Product “should we…?” → **DESIGN / DECISIONS**. “What works on devices?” → **CURRENT_STATE**.

## Dependencies

| Prerequisite | Why | Notes |
|--------------|-----|--------|
| Mesh **nr → nu → n3** + **N022** | NAT / Client dial; libp2p invest | Mobile default Client; **V027** call-scoped Wi‑Fi listen |
| Mesh **n4-media** `media_relay` | Group + undialable 1:1 hop | N021; hop-reachability **in-libp2p** (L1+) |
| HTTP Brief backend | Preferred org APIs / settle UX | Chain settle backup (N022) |
| Direct E2E messaging | Signaling + key wrap | Guests pairwise |
| Push Wave 1 + **`call_wake`** | Background ring | |
| Opus + SDL audio | Voice-first (V026) | Video deferred; H264 stack legacy until teardown |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| v0–a4 | Docs → signaling → WebRTC LAN → group SFU thin | **Done** (historical); WebRTC not ongoing path |
| **V026 / m1** | Libp2p-only **voice** | **Mobile LAN done**; desktop matrix open |
| m2 | Teardown WebRTC product path | **Next** |
| a5–a6 | Cap / polish / promote contracts | Pending |
