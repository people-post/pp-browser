# Call path resilience — phases

Ordering and checkboxes only. **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **Spec:** [DESIGN.md](DESIGN.md).

```
k0 ──┬── k1 (amp hygiene) ──┐
     └── k2 (call liveness) ┴── k3 (migrate) ── k4 (failover/reconnect) ── k6 (mobility policy)
k5 (network monitor) ─────────────────────────────┘
k7 (tests / hard lab / promotion) runs alongside every phase
```

k1 and k2 can run in parallel after k0. k5 is independent platform work and can start any time; k4's re-anchor and k6 consume it.

## k0 — Observability + dogfood root cause (M1)

- [x] Amp: `LinkEventListener` (Connected / Dropped+reason / PathChanged), posted off-strand; `MeshRuntime::AddLinkEventListener`; gtests (connected, dead, carrier-closed, path-changed) — `Suspect` moves to k1 with dead-peer detection
- [x] pp-browser: `MeshLink` logger subscribes and logs every event (`MeshLinkEventLog`)
- [x] Tag pp-cpp-amp with link events (v2.1.10) + pin in `cmake/PpCppAmp.cmake`
- [x] Fix path label: bundle records the bound link kind (`ICallMediaTransport::ActiveLinkKind`); `MediaPathKind()` = "circuit" whenever media rides a relay carrier; TX-only escalate uses the same truth
- [ ] Dogfood repro with logs from caller, answerer **and** relay R1; record the 10.0 s trigger in DECISIONS (K-ADR or note) and in [p2p-av-calls/CROSS_NETWORK_B25_B31.md](../p2p-av-calls/CROSS_NETWORK_B25_B31.md) style triage
- [x] Loopback red test `AmpCircuitCallMediaComposeTest.DISABLED_CallSurvivesRelaySilenceWithDirectPath` (relay leg + direct link, relay silent → today `peer link lost` on both legs); enable in k3/k4

**Exit:** every link drop in a dogfood log has a reason; the 10 s trigger is explained.

## k1 — Amp link hygiene (M2)

- [ ] Snapshot fields — transport kind (Direct / Punched / Carrier), remote endpoint, last-rx age (moved from k0; ship with this amp release)

- [ ] Drop Connected carrier link on carrier close (no Backoff linger)
- [ ] Drop inbound link on handshake error
- [x] Warm/hot dead-peer detection: keepalive echo + eviction past the cadence window (reason `connection-dead`; amp keepalive v2) — a separate `Suspect` event deferred until a consumer needs it
- [x] Liveness vs tier mismatch fixed: keepalive carries cadence, window = max(5 s, 5/2 × cadence) (amp docs/KEEPALIVE.md v2); product hot relaxed 2 s → 10 s, warm 60 s → 25 s
- [x] `MarkWarm` / `MarkHot` before link exists is remembered and applied on establish (amp `pending_keepalive_tiers_`)
- [x] Warm/hot links keep a cadence in both directions (inbound too); cold peers honour the announced cadence
- [ ] `MaybeLearnPath` after replay check (+ gtest: replayed packet from new address does not move the path)
- [ ] `LinkTable::Insert` on occupied key: no orphan in `by_id_`; `ScheduleDropLink` by LinkId (not key) so a replacement link is never dropped
- [ ] `idle_ttl`: implement or delete
- [ ] pp-cpp-amp release + pin

**Exit:** no link lingers in Backoff; dead warm/hot links evicted within 3 × interval.

## k2 — Call keeps its links alive (M3) — early mitigation

- [x] Relay links holding a circuit reservation are hot (`CircuitTunnelCoordinator` counts Reserved tunnels per relay; `ClearWarm` when the last one ends)
- [x] Product keepalive cadences from `AmpLinkConfig.h` (hot 10 s, warm 25 s — K008); tests share the product link config
- [ ] Call path set links (media link, relay outer links of a relayed path) `MarkHot` while Live; `ClearWarm` at hangup
- [ ] Call-scoped keepalive (K008): standby / relay outer links 10–15 s — Amp per-link interval override; device battery measurement picks the value
- [x] Renew circuit reservations every 10 s (15 s lease) from `BeginSession` until `StopMeshMedia` / teardown (`CallMediaBridge::ArmReserveRenewal`)
- [ ] Stop the post-Live Ensure/punch loop from running unowned — it becomes the k3 candidate producer
- [ ] Chat `WarmPeerByKey` ordering fixed via k1 (or reorder locally if k1 lags)

**Exit:** dogfood trace no longer loses the relay path while the call is live (even without migration).

## k3 — Make-before-break migration (M4)

- [ ] Bundle `PathSet` (active / standby / retiring) replacing single `bound_mux`; `ResolveLink` / `PeerLinkMissing` per path
- [ ] Hello `type:"migrate"` + `path_gen`; `DecideCallMediaInboundHello` accepts for MediaReady same call/epoch
- [ ] Second media/control channel roles; RX on any path (seq de-dupe); TX switch
- [ ] `path_release` / ack on control channel; old-path closes after release are not failures
- [ ] Direct planner events `PathCandidate` / `PathMigrated` / `PathLost`; replace `ConnectSucceeded`-while-Live "keep"
- [ ] Rewrite `TryUpgradeToDirectAsync`: migrate first, demote/standby after release (both roles)
- [ ] Interop: old peer rejects migrate → call continues on current path
- [ ] Loopback gtests: relay → punched with continuous seq; release ack; interop

**Exit:** dogfood relay → punched upgrade moves media; relay becomes standby.

## k4 — Media-liveness failover + reconnect (M5)

- [ ] Control-channel heartbeat (~500 ms) per path
- [ ] 1.5 s silence on active → switch to standby (K008)
- [ ] No standby → `Reconnecting` call status, relay re-anchor, 30 s window (K008); UI subtitle (i18n EN + zh-Hans)
- [ ] `peer link lost` no longer tears down while the path set / window allows
- [ ] TX-only escalate limited to initial connect
- [ ] Close p2p-av-calls a5 "Reconnect after brief network loss" (cross-link)

**Exit:** killing the active path mid-call → ≤ 2 s gap with standby; recover within window without.

## k5 — Network monitor (M7)

- [ ] `foundation/platform/NetworkMonitor` event API (transport, metered/expensive, change generation)
- [ ] Android `registerDefaultNetworkCallback`; iOS/macOS `NWPathMonitor`; Windows `NotifyIpInterfaceChange` + cost; Linux netlink (fallback poll)
- [ ] Reaction: suspect + keepalive burst + fast evict; reachability re-probe + advertise refresh
- [ ] Active call hook → k4 re-anchor
- [ ] Always bind mesh socket dual-stack `[::]` (K010); audit IPv4-only assumptions first
- [ ] `check_platform_ifdefs.sh` clean; platform code per PLATFORM_CODE.md

**Exit:** Wi-Fi ↔ cellular / sleep-wake mid-call recovers without user action.

## k6 — Mobility class + pair policy (M6)

- [ ] `domain/` `MobilityClassifier` (signals, hysteresis) — pure logic + gtests
- [ ] `caps.mobility` in invite/accept (no `v` bump) + `caps_update`; codec gtests
- [ ] `CallPathPolicy` pair table → punch / relay role (anchor vs standby) / standby priority; consumed by k2/k3/k4
- [ ] Relay: standby reservations best-effort with priority + refusal (K003); per-account standby cap; free standby, bill relayed bytes after failover (K009)
- [ ] Override: config key + `--mobility=`; docs/ops/CONFIGURATION.md
- [ ] (Later) user "Connection preference" setting

**Exit:** both ends compute the same policy; override flips behaviour on one machine.

## k7 — Tests, hard lab, promotion (continuous)

- [ ] Hard-lab wave: punch-then-relay-drop, NAT rebind mid-call, short NAT timeout, network flip
- [ ] Promote: CALLS.md (path set, migration, reconnect), WIRE_SCHEMAS (hello migrate, caps.mobility, control ops), MESH.md (link events, hygiene), amp docs/KEEPALIVE.md
- [ ] Fix doc drift found in survey: calls CURRENT_STATE "V001–V038", CALLS.md "through V038", H009 "plan only" header

## Later horizons

- Opportunistic multipath (duplicate send on two paths for lossy cellular)
- Path quality scoring (RTT/loss from heartbeats) choosing between two healthy paths
- N≥3: apply path sets to SFU legs
