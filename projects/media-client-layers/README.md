# Media client layers

**Status:** Planned — design agreed 2026-09-26; l0 landed (1:1 split)
**Owner:** Hongwei + agents
**Stable refs:** [CALLS.md](../../docs/architecture/CALLS.md), [MESH.md](../../docs/architecture/MESH.md), [THREADING.md](../../docs/architecture/THREADING.md), [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md)
**Related:** [peer-scoped-broadcast](../peer-scoped-broadcast/) (B001 broadcast ≠ call, B002 media tree, B007 ladder), [p2p-av-calls](../p2p-av-calls/) (topology / SoftMigrate), [call-path-resilience](../call-path-resilience/) (k3 path set), [hard-lab](../hard-lab/)

## One-line goal

Split the **client side** of real-time media into shared, feature-neutral layers — **reach a node**, **attach a relay session**, **capture / playback pipelines**, **device leases** — so that **calls** (1:1, group joiner) and **broadcast** (broadcaster, viewer) are sibling features that share mechanism but not policy.

## Why

- 1:1 was split this way first (`PeerReachCoordinator` → `CallMediaConnectCoordinator` → `CallMediaBridge`, branch `refactor/peer-reach-coordinator`). The same mechanisms (reach a hop, attach a `media_relay` session, reattach on loss) are duplicated or tangled inside `CallTopologyController` / `CallHopMigrateWorkflow`.
- Broadcast is **not a call** ([B001](../peer-scoped-broadcast/DECISIONS.md#b001--broadcast-is-not-a-large-group-call)) but its viewer path currently rides the group-call path: `AcceptLiveAnnounceJoin` → `CallTopologyController::OnAnnounceViewerJoined` → group `AttachLocalToSfuAsync`, sharing SoftMigrate flight state, attaching as a **publisher**, and never using the ticket / admit-or-redirect client RPC (`AmpBroadcastTransport::RequestTicket` / `RequestViewerAttach` have no product caller).
- No broadcast UI is wired yet, so separating now is cheap; after UI lands on the call-shaped path it is not.

## Scope

| In | Out |
|----|-----|
| Client (joiner / viewer / broadcaster) sides | Relay / hop serving side (ladder handlers, slot-win serving) — stays with the relay role |
| Neutral layers: reach, relay attach, capture / playback pipelines, device leases | Running a call and a broadcast at the same time (not needed now — **must not be blocked**, see [L003](DECISIONS.md#l003--device-leases-per-kind-exclusive-policy-for-now)) |
| `feature/broadcast` sibling of `feature/calls`; broadcaster + viewer workflows | Group → broadcast continuum (not needed) |
| Removing broadcast pieces from `feature/calls` | Viewer video in the first cut (needed later — API must not block it, [L004](DECISIONS.md#l004--stream-plans-carry-channels-generically)) |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Layers, what is shared vs separate, neutrality test, data plane |
| [PHASES.md](PHASES.md) | l0–l6 checklist (one PR each) |
| [DECISIONS.md](DECISIONS.md) | L001–L007 |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Where the work is today |
