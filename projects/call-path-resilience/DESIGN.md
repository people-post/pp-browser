# Call path resilience — design

**Phase status:** [CURRENT_STATE.md](CURRENT_STATE.md) only. **Order:** [PHASES.md](PHASES.md). **Rationale:** [DECISIONS.md](DECISIONS.md).

## Overview

A call has one **session** (call id, media key, epoch, sequence space) and, underneath it, a **path set**: the links that can carry its media right now — a direct or punched ADP link, a nested link over a relay circuit, later others. Paths come and go; the session must not. This project:

1. makes link changes **observable** (Amp events → app log and call layer),
2. keeps the links a call depends on **alive** while the call is live,
3. moves media between paths **make-before-break** and releases a path only by **explicit agreement**,
4. **fails over** on media silence instead of tearing the call down,
5. **detects network changes** on every platform and re-anchors,
6. picks the path policy per call from each endpoint's **mobility class**, detected automatically and exchanged in call caps.

## Goals

- A 1:1 call survives: relay → punched upgrade, punched path death, relay death with a punched standby, NAT rebinding, Wi-Fi ↔ cellular handoff, laptop sleep/wake.
- Media gap on path loss ≤ ~2 s when a standby path exists; "Reconnecting…" (not hangup) for up to a reconnect window when none does.
- Every link drop in the log has a reason.
- No new L4 protocol id; wire changes are optional fields / message types in existing call-control and call-media hello.

## Non-goals

- Simultaneous multipath media beyond the migration overlap window.
- ICE/TURN; physical motion sensing; asking users to classify themselves.
- N≥3 SFU SoftMigrate (p2p-av-calls owns it; this project only must not regress it).

## Current state (2026-09-24)

Survey of pp-browser + pp-cpp-amp at `af5c8af60` / amp `v2.1.9`. File refs are for orientation; headers on disk win.

### Dogfood trace (answerer)

| T (s) | Event |
|------:|-------|
| 0 | Inbound call-media hello over nested carrier (relay R1 `Qmd2m4…`); leg Live; audio received |
| +3.5 | Cold punch to caller succeeds (`CallMedia peer connected … path=punched`); planner `Direct keep` — media **stays on the carrier** |
| +13.5 | RX audio stops (exactly 10.0 s after punch); TX continues |
| +20.8 | `CallMediaLeg teardown … reason=amp call-media: peer link lost` |

Probable tail (code-consistent, not yet proven by link logs): relay path goes silent → answerer↔R1 ADP link not warm → evicted after `kAliveTimeoutMs` 5 s → carrier orphaned → nested link evicted → `ResolveLink` falls back to the punched ADP link (`FindByPeerId` prefers ADP), mux differs → `PeerLinkMissing` → teardown. **The 10.0 s trigger is unexplained**; candidates: reserve TTL 15 s from the pre-punch seed park (timing fits; this code only closes the reserve channel), caller- or relay-side behaviour after the caller's own punch success, relay splice loss. k0 exists to answer this with link events from all three nodes.

### Gaps

| Area | Today | Where |
|------|-------|-------|
| Link events | None. Only `PeerConnectedListener` (connected, by PeerId) and an unused L1 `Connection::OnPathChange`. App polls `FindLink` / `IsConnected` per tick. Amp has no logging. | `amp/link/PeerLinkManager.h`, `CallMediaLegCoordinator::TickDeadlines` |
| Bundle binding | Bundle pinned to first link's mux (`bound_mux`), set once; different link to same peer ⇒ "missing". Second hello for same call ⇒ `RejectBusy`. Any Media/Control close ⇒ FailLeg. | `CallMediaLegCoordinator.cpp` (`NoteBoundLink`, `ResolveLink`), `CallMediaBundleLogic.cpp` |
| Planner | `ConnectSucceeded` while Live = no-op "keep". Ensure/punch loop keeps running after Live with no purpose. No path-upgrade / path-lost events. | `CallDirectPlannerLogic.h`, `CallMediaBridge.cpp` |
| Path label | Answerer shows "Punched" while media is on the circuit (`IsConnected` counts carrier links; answerer never installs inbound carriers in the hop registry). | `CallMediaBridge::MediaPathKind` |
| Upgrade helper | `TryUpgradeToDirectAsync` punches then cancels the tunnel **before** moving media (break-before-make); no production caller. | `AmpCircuitHopReach.cpp` |
| Relay ownership | L3.25c says "request circuit close when safe"; "safe" undefined. Reserves have 15 s TTL and are never refreshed during a call. | `media-hop-reachability/HOLE_PUNCH.md`, `CallMediaPlane.cpp` |
| Keepalive | Calls never `MarkHot`. Chat `MarkWarm` runs before the link exists (no-op). Only outbound links send keepalives. | `MeshDeliveryOrchestrator.cpp`, `PeerLinkManager::MaybeSendKeepalives` |
| Dead links | Warm/hot links on a dead path are never evicted (only on received Close). Connected carrier link whose carrier closes → Backoff, never dropped. Inbound handshake error → Backoff, never dropped. `idle_ttl` unused. | `PeerLink.cpp`, `PeerLinkManager::Tick` |
| Failover | Only TX-only escalate (RX total 0, non-circuit, once). No RX-stall-after-Live handling; `TryRecoverViaSfu` uncalled. | `CallTxOnlyEscalateLogic.h` |
| Network change | No OS listener on any platform. `NetworkConnectivity::IsOnWifi` has no production caller. Reachability probe only at startup / manual. | `foundation/platform/NetworkConnectivity.*`, `ReachabilityEngine` |
| Path learning | ADP follows an authenticated packet's new source (A003) **before** the replay check — a replayed packet can redirect the path. | `amp/src/L1/Connection.cpp` `MaybeLearnPath` |

What already works and is reused: ADP association migrates on the **remote** address change without re-handshake (A003); call-media frames are link-portable (AAD `call-media|call_id|epoch|seq|channel`, no path term) and the jitter buffer de-dupes on seq; ADP and nested-carrier links to one PeerId may coexist (A024/A026).

## Model

```
CallSession (call_id, media_key, epoch, seq)            ← unchanged across paths
  └─ PathSet
       ├─ active   : Path{link_id, kind, gen, bound channels}   ← sends here
       ├─ standby  : Path{…}                                    ← warm, receives, heartbeats
       └─ retiring : Path{…}  (drain until release acked)
Path kind: Direct (dialed ADP) | Punched (ADP via ACP burst) | Relayed (nested over circuit)
```

- A **path** is (link, its bound control + media channels, `path_gen`). `path_gen` increments per new path in the call; both ends agree on it through the migrate hello.
- The receiver accepts media on **any** path of the set (seq de-dupe), so overlap is harmless.
- The sender sends on **active** only (overlap: may duplicate on old+new during the switch window, ≤ 1 s).

## Mechanisms

### M1 — Link events (Amp) → k0

`PeerLinkManager` gains a multi-listener `LinkEventListener` (posted like `PeerConnectedListener`, never invoked inside a link callback):

| Event | Payload |
|-------|---------|
| `Connected` | link id, dial key, peer id, transport kind (Direct/Punched/Carrier), remote endpoint |
| `Dropped` | link id, peer id, **reason** (dead-ADP, closed-by-peer, carrier-closed, dual-dial-lost, handshake-failed, handshake-timeout, aborted, displaced, local-close), last-rx age |
| `PathChanged` | link id, old/new remote endpoint (from `Connection::OnPathChange`) |
| `Suspect` | link id, silence ms (warm/hot link past liveness threshold — see M2) |

Snapshots gain transport kind, remote endpoint and last-rx age. pp-browser logs every event (`[MeshLink]` logger) — the log file then answers "which link died and why".

### M2 — Link hygiene and liveness (Amp) → k1

- Drop (scheduled) a Connected carrier link when its carrier closes, and an inbound link whose handshake errors — no lingering Backoff links.
- Warm/hot dead-peer detection: after `3 × interval` without authenticated RX despite keepalives → `Suspect`, then drop with reason dead. Hot interval during calls: see M3.
- `MarkWarm`/`MarkHot` on a key with no link yet is remembered and applied on establish (fixes the chat ordering no-op).
- Inbound links may send keepalives when marked hot (a mobile answerer's inbound link must keep its own NAT mapping).
- Path learning after the replay-window check; optional path validation (probe the new endpoint before switching) for non-call links.
- `LinkTable::Insert` on an occupied dial key drops/rebinds instead of orphaning in `by_id_`.
- Remove or implement `idle_ttl`.

### M3 — Call keeps its links alive → k2

While a call is Live, every link in its path set **and the outer link carrying a relayed path** (answerer↔R1, caller↔R1) is **hot** (K008): the active path is kept alive by media + the M5 heartbeat; standby and relay outer links send keepalives every 10–15 s (cellular NAT timeouts can be ≤ 30 s). Circuit reservations used by the call are refreshed before TTL while the call is live and released at hangup. Hot is cleared at hangup (`ClearWarm` finally has a caller).

### M4 — Make-before-break migration → k3

Initiator: the side whose local PeerId wins `LocalWinsCallMediaGlare` (antisymmetric, already used for glare). The other side may *request* migration (it discovered the better path) via call-control; the winner then drives.

1. Candidate path link is Connected (M1 event) and passes policy (M6).
2. Initiator opens control + media channels on the candidate link and sends hello `{type:"migrate", call_id, media_epoch, path_gen: n+1}`.
3. Responder accepts (bundle MediaReady, same call_id/epoch, `path_gen` = current+1) and binds the new channels as **standby**; replies `migrate_ok`.
4. Initiator switches TX to the new path; responder switches TX on first valid media frame received on the new path (or on `migrate_ok` ack echo) — whichever first.
5. After both sides have RX on the new path for ≥ 1 s: initiator sends `path_release {path_gen: n}` on the new control channel; responder acks; both close old channels **quietly**. Old-path channel closes after release are not failures.
6. Relay release rule (K002): the old relayed path becomes **standby** (not closed) according to M6 policy; the tunnel is cancelled only if policy says `release` and only after step 5.

Failure at any step before 4 → drop candidate, stay on current path. `TryUpgradeToDirectAsync` is rewritten on top of this (migrate first, demote after release).

### M5 — Media-liveness failover and reconnect → k4

- Each path's control channel carries a heartbeat (≈ 500 ms; tiny) so silence is detectable during DTX / muted mic.
- Active path: no heartbeat and no media for **1.5 s** (K008) → switch TX to standby (step 4 of M4 without the handshake — standby is already bound), log reason.
- No usable standby → **re-anchor**: the call enters `Reconnecting` (UI "Reconnecting…", a5 in p2p-av-calls), dials/re-reserves via relay (outbound always passes NAT), punches later per policy. Reconnect window **30 s** (K008) before the call fails.
- `peer link lost` stops being an immediate teardown: link loss removes the path from the set; only an empty set past the reconnect window ends the call.
- Supersedes the once-per-call TX-only escalate for 1:1 (kept as the initial-connect fallback).

### M6 — Mobility class and pair policy → k5, k6

**Class = network-attachment stability, not physical movement.** Per endpoint, recomputed on every network event, with hysteresis:

| Signal | Source | Weight |
|--------|--------|--------|
| Interface type (cellular / Wi-Fi / ethernet) | Android `NetworkCapabilities`, Apple `NWPathMonitor`, desktop interface enumeration | Primary |
| Metered / expensive (catches hotspot tethering) | Android `NOT_METERED`, Apple `isExpensive` / constrained, Windows network cost, NetworkManager `metered` | Primary |
| Network change events / count per 10 min | same monitors (M7) | Churn |
| Own observed address changes; remote `PathChanged` counts | dial-back observed addr, M1 `PathChanged` | Churn (strongest, measured) |
| Sleep/wake, VPN up/down | OS power / interface events | Churn |

Classes: `Stationary`, `Mobile`, `Unknown` (treated as Mobile for relay retention, Stationary for punch attempts). Override: config key + `--mobility=stationary|mobile` for dogfood / hard-lab; optional user preference "Connection preference: Automatic / Prefer direct / Prefer stable" (later; UI copy per UI_DESIGN_SYSTEM).

Exchanged as optional `caps.mobility` (`"stationary"|"mobile"|"unknown"`) in CallInvite/CallAccept caps — **no `v` bump** (`ReadPeerCaps` zeroes caps on unknown `v`). Updated mid-call through a call-control `caps_update` if the class flips. Both sides compute the same **pair policy**:

| Pair (A, B) | Primary path | Punch | Relay | Hot interval |
|-------------|--------------|-------|-------|--------------|
| Pair (A, B) | Primary path | Punch | Relay | Standby priority |
|-------------|--------------|-------|-------|------------------|
| Stationary, Stationary | Direct or punched | Yes, early | Standby requested (best-effort) | Low (direct) / Medium (punched) |
| Mobile, reachable Stationary (public / port-mapped) | Direct dial from mobile side | Not needed | Standby requested | High |
| Mobile, NATed | **Relayed (anchor)** | Opportunistic upgrade only if both non-metered and not cellular; demote on first loss | **Anchor — required; never released during the call**; after an upgrade it stays as standby | High |
| Any, Unknown | as Mobile for relay priority, as Stationary for punch (K004) | | | High |

Relay **standby is requested for every call but best-effort** — a loaded relay may refuse, highest priority first (K003). With a standby, primary loss is a ≤ 2 s switch; without one, M5 re-anchor (few seconds, within the 30 s window). Either way misclassification costs latency, never the call. Standby is free to the caller, capped per account; relayed bytes after failover are billed (K009). Keepalive cadence per K008.

### M7 — Network change handling → k5

`foundation/platform/NetworkMonitor` (event API, replaces the one-shot `NetworkConnectivity` poll): Android `ConnectivityManager.registerDefaultNetworkCallback`, iOS/macOS `NWPathMonitor`, Windows `NotifyIpInterfaceChange` + network cost, Linux netlink `RTMGRP_IPV4/6_IFADDR` (fallback: `getifaddrs` diff every 2 s). Event → `domain/mesh` reaction:

1. Mark all ADP links **suspect** and send an immediate keepalive burst (hot and warm); evict on no reply within 2 s (M2).
2. Re-run the reachability probe (observed address, UPnP) and refresh advertised addrs / punch candidates.
3. Active calls: trigger M5 re-anchor if the active path is suspect; recompute mobility class; `caps_update` if changed.
4. No socket rebind: the mesh socket is always dual-stack `[::]` (K010), so a family change (e.g. IPv6-only cellular with NAT64) needs no action.

## Wire summary (promote to `docs/contracts/WIRE_SCHEMAS.md` when shipped)

| Carrier | Change | Compat |
|---------|--------|--------|
| Call-media hello JSON | `type:"migrate"`, `path_gen`; replies `migrate_ok` / reject | Old peers reject unknown type → no migration, call stays as today |
| Call-media control channel | `path_release{path_gen}` / ack; `hb` heartbeat | Ignored by old peers (unknown op) |
| Call-control caps | optional `caps.mobility`; `caps_update` message | Unknown key ignored; missing ⇒ `unknown` |
| L4 protocol ids | none | A028/N030 freeze respected |

## Failure UX

| Situation | UI |
|-----------|----|
| Migration in progress | Nothing (path label updates after release) |
| Active path lost, standby switch ≤ 2 s | Brief audio gap; call details path label changes |
| Re-anchor | "Reconnecting…" subtitle; timer keeps running |
| Reconnect window exhausted | Existing call-failed flow, reason "Connection lost" |

Path label must reflect the **active path** (fixes the "Punched while on circuit" mislabel).

## Testing

- Amp gtests: every drop reason emits one `Dropped`; carrier-closed / handshake-error links dropped; warm/hot dead detection on `VirtualClock`; replayed packet from new address does not move the path.
- pp-browser loopback (existing call-media partition fixtures): relay → punched migrate with continuous seq; kill active path with standby present (gap ≤ 2 s); kill all paths → Reconnecting → recover; old-peer interop (migrate rejected, call continues).
- Hard lab: new wave — punch then drop relay; NAT rebinding mid-call (change SNAT mapping); short UDP NAT timeout (cellular-ish); network flip on one node. Mobility override flag drives both policies on one machine.
- Dogfood: `scripts/dev/pp_dogfood.sh -- --debug` on both ends; link events make the 10 s question answerable.

## Open questions

Resolved 2026-09-24: keepalive cadence + failover timing (K008), standby best-effort with priority (K003), standby billing (K009), dual-stack socket (K010), Unknown class (K004), migration driver = glare winner (M4).

Remaining:
- Standby keepalive interval within 10–15 s — pick from a device battery measurement (K008).
- Relay caps: concurrent reservations per relay and per-account standby cap values (p2p-mesh relay scope / pricing).
