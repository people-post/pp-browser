# Call path resilience — decisions

Prefix **K**. Status lives in [CURRENT_STATE.md](CURRENT_STATE.md); spec in [DESIGN.md](DESIGN.md).

---

## K001 — A call binds to a path set, not a link

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** A call-media bundle owns a **path set** (active / standby / retiring), each path = link + its bound channels + `path_gen`. The session (call id, media key, epoch, seq) is independent of paths. Link loss removes a path; only an empty set past the reconnect window ends the call.  
**Rationale:** Today the bundle is pinned to the first link's mux (`bound_mux`); any link change is a teardown (dogfood 2026-09-24: relay path lost → `peer link lost` while a punched link to the same peer was Connected). Frames are already link-portable (AAD has no path term) and the jitter buffer de-dupes on seq, so multi-path receive costs little.  
**Alternatives rejected:** Re-dial a fresh session on path change (audible gap, new key/epoch handshake, glare); rebinding `bound_mux` in place (no overlap → break-before-make).  
**Cross-link:** [A024](../adp/DECISIONS.md) nested carrier; [V038](../p2p-av-calls/DECISIONS.md#v038--n2-circuit-for-nat-softmigrate-reserved-for-n3) N=2 path order.

---

## K002 — Make-before-break; a path is released only by explicit two-sided agreement

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** Migration opens the new path, switches TX, and releases the old path only after both sides have RX on the new path and a `path_release` is acked. Timers never release a path that carries or backs a live call. The relay path, when superseded, becomes **warm standby** (keepalive-only), not closed (see K003).  
**Rationale:** Supplies "safe" for L3.25c "request circuit close when safe". The 2026-09-24 trace lost media 10 s after the punch while the planner kept media on the relay — exactly the unowned-relay failure. The existing `TryUpgradeToDirectAsync` cancels the tunnel before moving media.  
**Alternatives rejected:** Close relay on punch success (break-before-make); close after fixed grace (races slow peers, loses media on asymmetric switch).  
**Cross-link:** [media-hop-reachability HOLE_PUNCH.md](../media-hop-reachability/HOLE_PUNCH.md) L3.25c; H005 billing.

---

## K003 — Relay standby requested for every call; best-effort, relay may refuse

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** Two relay roles are distinct:
- **Anchor** (relay is the primary path, e.g. Mobile↔NATed): required for the call; a refusing relay is replaced by the next rendezvous candidate as today; never released during the call.
- **Standby** (relay behind a direct/punched primary): **requested for every call**, but the relay **may refuse** under load (reservation/link caps). Priority when full: Mobile or Unknown pairs, then punched primaries, then Stationary↔Stationary direct.

Without a standby, primary-path loss goes to k4 re-anchor (`Reconnecting…`, relay dial) — a few seconds' gap instead of ~2 s, still within the reconnect window (K008).  
**Rationale:** Keeps the core promise — misclassification (K004) or punched-path death never costs the call — without making relays a hard per-call dependency or a load cliff during regional blips. Standby carries no media until failover (a reservation + keepalives, a few hundred bytes/min).  
**Alternatives rejected:** Guaranteed standby for every call (relay capacity becomes a hard limit on call count); standby only for fragile pairs (a stationary laptop that sleeps/roams loses the seamless switch for no capacity reason).  
**Cross-link:** billing K009; capacity limits [p2p-mesh relay scope](../p2p-mesh/RELAY_SCOPE.md).

---

## K004 — Mobility is detected automatically as network-attachment stability

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** Each endpoint classifies itself `Stationary | Mobile | Unknown` from interface type, metered/expensive, network-change events, own observed-address churn and remote `PathChanged` counts, with hysteresis. No user prompt. Dev override via config / `--mobility=`; optional user preference ("Automatic / Prefer direct / Prefer stable") later.  
**Rationale:** What breaks calls is address/NAT-mapping stability, not motion: a still phone on cellular CGNAT rebinds; a laptop on a phone hotspot looks like Wi-Fi. Users cannot judge this; OS signals can. K003 makes errors cheap.  
**Unknown** (old peers without `caps.mobility`, platforms before k5): Mobile for relay-standby priority, Stationary for punch attempts. Safe because a failed punch/primary falls back to standby or re-anchor (K003).  
**Alternatives rejected:** User-selected mode ("I'm not moving fast") — wrong in the failing cases; motion/location sensors — irrelevant signal, permission + privacy cost.

---

## K005 — Mobility on the wire as optional `caps.mobility`

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** Add optional `caps.mobility` to CallInvite/CallAccept caps and a `caps_update` call-control message for mid-call flips. Do **not** bump `caps.v`. Both ends compute the same pair policy deterministically.  
**Rationale:** `ReadPeerCaps` zeroes caps when `v` exceeds the known version, which would silently disable `media_relay` on old peers. Unknown keys are ignored per WIRE_SCHEMAS unknown-field policy.  
**Alternatives rejected:** Amp ch0 capability payload (nested carrier links skip ch0; mobility is call policy, not link capability).

---

## K006 — No new L4 protocol id; migration rides existing call-media / call-control

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** Migration uses hello `type:"migrate"` + `path_gen` and control-channel ops (`path_release`, `hb`); mobility uses call-control caps. No new protocol id.  
**Rationale:** L4 kinds are frozen (A028/N030). Old peers reject the unknown hello type → call continues on its current path (graceful).  
**Cross-link:** [L4_PROTOCOL_KINDS.md](../../docs/contracts/L4_PROTOCOL_KINDS.md).

---

## K007 — Amp emits link events; the app logs them

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** `PeerLinkManager` exposes a multi-listener link event API (Connected / Dropped+reason / PathChanged / Suspect), posted off-strand. Amp stays logging-free; pp-browser logs events under `[MeshLink]` and the call layer consumes them instead of per-tick `FindLink` polling.  
**Rationale:** Dogfood 2026-09-24 could not tell which link died or why — Amp drops are silent and the app notices ~7 s later by polling. Events posted off-strand avoid the re-entrancy class fixed in amp v2.1.9 (drop inside establish callback).  
**Alternatives rejected:** Logging inside Amp (library policy: no product logging); keep polling (late, reasonless).

---

## K008 — Liveness cadence and failover timing

**Date:** 2026-09-24  
**Status:** Accepted (design) — standby interval to be confirmed by device measurement  
**Decision:**
- **Active path:** kept alive by media plus a control-channel heartbeat (~500 ms); no extra keepalive tier needed.
- **Standby path and relay outer links:** hot keepalive every **10–15 s** (pick after measuring standby battery cost on one Android + one iOS device); keepalive burst on network change / suspicion.
- **Failover:** active path with no heartbeat and no media for **1.5 s** (≈ 3 missed heartbeats) → switch to standby.
- **Reconnect window:** **30 s** of `Reconnecting…` with an empty path set before the call fails.

**Rationale:** During a call the radio is already awake for 20 ms audio frames, so active-path keepalives are free; the battery cost is only on standby. Heartbeat-based silence avoids false failover on mute / Opus DTX. 30 s covers tunnels / elevators better than 20 s.  
**Amendment (2026-09-24, k2):** a keepalive interval only keeps a link alive if it beats the **peer's** ADP liveness window (`kAliveTimeoutMs` 5 s) — the far end of our link is usually a cold inbound link that evicts us after 5 s without RX, whatever tier we hold. Interim product hot keepalive was **2 s**. **Resolved (k1, pp-cpp-amp keepalive v2):** keepalives carry the sender's cadence, the peer widens its window to 5/2 × cadence and echoes, warm/hot links are evicted when silent past it. Product cadences now: **hot 10 s** (reservations / standby — cellular NAT), **warm 25 s** (chat) — `AmpLinkConfig.h`. Standby battery measurement may still tune hot within 10–15 s.  
**Alternatives rejected:** Uniform 5 s keepalive on every call link (standby radio wakeups on cellular); media-silence-only failover (mute ⇒ false switches).

---

## K009 — Relay standby billing: free, capped per account; charge on failover

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** A standby reservation is free. Relayed bytes after a failover are billed as brokered relay use (H005 unchanged for media). Abuse control: a per-account cap on concurrent standby reservations, and a standby reservation is valid only while its call is live (released at hangup).  
**Rationale:** Standby costs the relay almost nothing (no media); per-minute quoting would add payment traffic to every call.  
**Alternatives rejected:** Per-minute standby fee; free-up-to-N-minutes (complexity for negligible cost).  
**Cross-link:** [H005](../media-hop-reachability/DECISIONS.md) brokered relay billing; [pricing](../pricing/).

---

## K010 — Always bind the mesh socket dual-stack

**Date:** 2026-09-24  
**Status:** Accepted (design)  
**Decision:** Bind the Amp UDP socket to `[::]` with `IPV6_V6ONLY=0` whenever the OS supports it, instead of choosing IPv4-only at startup when no global IPv6 exists. No Amp `Endpoint` socket-swap API.  
**Rationale:** The startup choice breaks a device that starts on an IPv4-only network and moves to IPv6-only cellular (NAT64); a dual-stack socket handles both families, so the choice never needs revisiting after a network change.  
**Alternatives rejected:** Endpoint IO swap + rebind on family change (new Amp API for a case dual-stack removes).  
**Still open:** Verify no code path depends on the IPv4-only bind (advertised addrs, dial-back observed parsing) before switching.

---

## K011 — A lost path with another live link to the peer rebinds quietly

**Date:** 2026-09-28  
**Status:** Accepted (k7)  
**Decision:** When a call's active path is lost with no standby but the peer is still Connected on another link, the glare winner (offerer) migrates the call there immediately; `on_path_lost` (planner Reconnecting, UI `Reconnecting…`) is held for **1 s** and raised only if the call still has no path. The lost path's placeholder is dropped on the reconnect switch — never kept as retiring or standby. A bundle created from the peer's hello takes the complementary role; a local leg joining it stamps its own role.  
**Rationale:** Hard lab (cone NAT, k7 `punch` phase): a simultaneous punch brings up two associations and Amp's dual-dial election drops one after both were published Connected; the call had often bound the loser. k4 recovered in ~20 ms, but through `Reconnecting…` — a visible flap on a healthy call. In 2 of 11 lab runs the recovery waited the full second because neither end drove: the offerer, having joined the bundle the answerer's early hello created, still held that bundle's default answerer role. The placeholder, unbound, resolved by alias to the new link and passed for a live channel-less standby a later failover would have switched onto.  
**Alternatives rejected:** Make only one end dial in a punch burst (Amp) — loses the case where only one direction gets through (symmetric NAT on one side). Debounce `Reconnecting…` in the UI only — the transport would still wait for the bridge's re-anchor instead of using the link it already has.

---

## K012 — Network changes: re-validate links, never probe into no route

**Date:** 2026-09-28  
**Status:** Accepted (k5)  
**Decision:** A platform `NetworkMonitor` (one backend per OS) reports material changes — online, transport, cost, or the fingerprint of the default-route interfaces and their addresses. The mesh owner fans them out: online on a new attachment (or back online) → Amp probes every direct link and drops the silent ones after 2 s (pp-cpp-amp v2.7.0), dial backoffs cleared, reachability re-probed 2.5 s later; calls re-anchor once links settle and a relayed call's upgrade punches start over. Going offline does nothing; a cost / label-only change does nothing yet (k6 consumes it).  
**Rationale:** Links outlived the network they were built on for their liveness window — up to 50 s for a hot link — while calls waited on them. Probing from the new address costs one packet per link, moves the peer's path when the peer can still reach us (the relay link survives a NAT rebind), and turns a dead path into an ordinary drop the k4 machinery already handles. Probing while offline would make every send fail and drop every link at once, destroying links a short outage (elevator, tunnel) would not have killed. The fingerprint covers default-route interfaces only, so local container bridges and VPN-less side interfaces do not look like network changes.  
**Alternatives rejected:** Rebind the mesh socket on change (not needed: dual-stack `[::]`, K010). Tear links down proactively on every change (kills links that would have moved). Poll-only detection (slow, battery) except as the Linux fallback.

