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

- [x] Snapshot fields — `LinkPathKind` (Direct / Punched / Carrier), live remote endpoint, last-rx age; `LinkEvent::path_kind`; `MeshLink` log prints `path=` (amp v2.4.0)

- [x] Drop a carrier-backed link on carrier close in any phase but Handshaking / Dialing (no Backoff linger)
- [x] Drop inbound link on handshake error (`HandshakeFailed`)
- [x] Warm/hot dead-peer detection: keepalive echo + eviction past the cadence window (reason `connection-dead`; amp keepalive v2) — a separate `Suspect` event deferred until a consumer needs it
- [x] Liveness vs tier mismatch fixed: keepalive carries cadence, window = max(5 s, 5/2 × cadence) (amp docs/KEEPALIVE.md v2); product hot relaxed 2 s → 10 s, warm 60 s → 25 s
- [x] `MarkWarm` / `MarkHot` before link exists is remembered and applied on establish (amp `pending_keepalive_tiers_`)
- [x] Warm/hot links keep a cadence in both directions (inbound too); cold peers honour the announced cadence
- [x] `MaybeLearnPath` / liveness only for a fresh packet (replay window for data, newest wire timestamp for control) — gtest `ReplayedPacketFromANewAddressDoesNotMoveThePath`
- [x] Drops target a link by `LinkHandle`, never "whatever holds the key"; ADP and nested links may share a key (A024) and neither waits on the other's handshake (hard-lab COLD regression found and fixed on the way: `NestedEstablishDoesNotWaitOnAnAdpDialUnderTheSameKey`)
- [x] `idle_ttl`: deleted (unused)
- [x] `RequestDropLink(dial key | PeerId)` for stale links the product detects (B39, PR #223; reason `requested`)
- [x] Hop applies the carrier policy to the dialer's leg of a call-media bridge (one-way stall root cause, 2026-09-24)
- [x] Reliable delivery for nested Reliable-class channels over a best-effort carrier: end-to-end retransmit chosen over dual outer lanes (no relay change) — amp `CarrierLane` (ADR_LINK_PLANE §11, v2.5.0): sequenced Reliable-class frames, cumulative + selective acks, adaptive RTO, in-order release; a frame unacked after 10 sends drops the nested link (`connection-dead`); probe-negotiated, older peers unchanged
- [x] Inbound link dial key renders the assoc id as broken hex (`inbound:=:>7=;…`) — fix the nibble encoding (amp `c36bf10`)
- [x] Inbound adopt keeps an ephemeral `amp:burst:N:` dial alias on the carrier link (amp `c36bf10`)
- [x] Reach loop labels a carrier-only link "punched"/"direct" — `IDialRegistry::IsConnectedDirect`
- [x] Close an ADP association at once when the socket reports EHOSTDOWN / ENETUNREACH for its peer (`kDatagramSendUnreachable` → `TransportFailed`) (#215 B39 suggestion a)
- [x] pp-cpp-amp release + pin (**v2.4.0** hygiene, **v2.5.0** reliable lane)

**Exit:** no link lingers in Backoff; dead warm/hot links evicted within 3 × interval.

## k2 — Call keeps its links alive (M3) — early mitigation

- [x] Relay links holding a circuit reservation are hot (`CircuitTunnelCoordinator` counts Reserved tunnels per relay; `ClearWarm` when the last one ends)
- [x] Product keepalive cadences from `AmpLinkConfig.h` (hot 10 s, warm 25 s — K008); tests share the product link config
- [ ] Call path set links (media link, relay outer links of a relayed path) `MarkHot` while Live; `ClearWarm` at hangup — **deferred to k3** (2026-09-27): a link carrying media is kept alive by the media; hot only matters for idle standby paths, which k3 creates. Needs a tier arbiter first — tiers are per key and not refcounted, so a call's `ClearWarm` would strip chat's warm / a reservation's hot
- [ ] Call-scoped keepalive (K008): standby / relay outer links 10–15 s — Amp per-link interval override; device battery measurement picks the value
- [x] Renew circuit reservations every 10 s (15 s lease) from `BeginSession` until `StopMeshMedia` / teardown (`CallMediaBridge::ArmReserveRenewal`)
- [x] ~~Stop the post-Live Ensure/punch loop~~ — obsolete: reach is one-shot, owned by `CallMediaConnectCoordinator` and finished at MediaReady. The k3 candidate producer is new work (on the Connectivity owner)
- [x] Answerer's ~12 s wait (PR #223 call #5): the seed park before CallAccept only settled early when **all** seeds were Connected, so one unreachable seed cost the full 12 s deadline. Now ≥1 Connected arms a 2 s grace for the rest (`DecideSeedParkStep`, `CircuitRendezvousCoordinator`); CallAccept stays behind the park (the answerer must be ServeDial-reachable before the offerer dials)
- [x] Chat `WarmPeerByKey` ordering fixed via k1 (amp `pending_keepalive_tiers_`)

**Exit:** dogfood trace no longer loses the relay path while the call is live (even without migration).

## k3 — Make-before-break migration (M4)

Slices (2026-09-27 survey of `CallMediaLegCoordinator` — one `Bundle` pins one `bound_mux` + three channel slots):

- [x] **k3-0** A second hello for a live call is refused on its own channel — it used to evict the live inbound control first (the Close reached the peer and failed the call), so a migrate hello to today's code would have killed it (`SecondHelloForALiveCallLeavesTheCallAlone`)
- [x] **k3-1** Bundle → `Path{LinkHandle, mux, kind, gen, control ×2, media}` (`active` only for now); a bound path resolves its link by handle, not the alias / PeerId (A024 links coexist); per-path `DropPathRole` / `PathOwnsRole` / `PathLinkMissing` / `MuxAliveForPath`; no behaviour change
- [x] **k3-2** `CallMediaLegCoordinator::MigrateLeg(leg, LinkHandle)`: `migrate` / `migrate_ack` / `path_release` / `path_release_ack` on a control channel opened on the chosen link's own mux; the responder adopts the candidate as standby; each side switches TX when its end of the new media channel is bound; old path released after RX on the new one (1–5 s); 5 s migrate timeout (older peers ignore `migrate`); glare winner (offerer) drives; per-channel seq de-dupe (`CallMediaSeqWindow`); `on_path_changed` callback. Wire: [AMP-CHANNEL.md § Path migration](../../docs/contracts/AMP-CHANNEL.md). Nothing calls it in the product yet (k3-3)
- [x] **k3-3** Product wiring. The transport moves a relayed call onto a Connected direct link to the same peer by itself (driver side, IO tick, 10 s backoff — `SetAutoMigrateToDirect`); the offerer, Live on a relayed path, punches for one via the circuit's relay as introducer at +3 / +20 / +60 s (`CallMediaBridge::ArmDirectUpgrade` → `PeerReachCoordinator::UpgradeToDirect` → `TryUpgradeToDirectAsync`, now punch-only: the circuit is never demoted — the migration releases the call's relayed path; blocking `TryUpgradeToDirect` removed). Planner `PathMigrated` (Live stays Live) replaces the repeated-`ConnectSucceeded` keep; path label follows the bound link (punched after an upgrade). `CallSurvivesRelaySilenceWithDirectPath` enabled (k0's red test). Hard-w5 green; the lab NATs never punch, so the upgrade misses there — a punchable relayed call is k7
- [x] **k3-4** TX-only escalation make-before-break: the bridge builds a circuit under the live call (reach, `exclude_direct`) and moves it there (`ICallMediaTransport::MigrateTo(Relayed)`); only a failure falls back to Detach + BeginSession. Planner `PathMigrated` takes DegradedTxOnly back to Live (chrome `DirectConnected`). Either end may migrate — simultaneous attempts: the glare winner's goes ahead, the other yields. A call moved onto the relay is never auto-migrated back onto the direct link it left. "No media" stays **no frames**, not no audio: a muted / micless peer still sends silence frames (`MutedCallStillSendsFrames`). Amp v2.6.0 `FindConnectedLinkByPeerId(peer, transport)`

Checklist:

- [x] Bundle path set (active / standby / retiring) replacing single `bound_mux`; link resolution / loss per path (k3-1, k3-2)
- [x] `migrate` (its own control type on a new channel, not a second `hello`) + `path_gen`; accepted for a MediaReady call with the same epoch and the next `path_gen` (k3-2)
- [x] Second media/control channels per path; RX on any path (seq de-dupe); TX switch (k3-2)
- [x] `path_release` / ack on control channel; old-path closes after release are not failures (k3-2)
- [x] Planner `PathMigrated`; `ConnectSucceeded`-while-Live no longer carries path changes (k3-3). A candidate needs no planner event — the transport takes it when the link appears; `PathLost` is k4
- [x] `TryUpgradeToDirectAsync` punch-only, migration moves the call; the relay is left as it is (standby policy is k4 / k6) (k3-3)
- [x] Interop: an older peer ignores `migrate` → 5 s timeout, call continues on its path (k3-2)
- [ ] Trust an existing Connected link for a call only if its remote endpoint is in the call's current candidate set (invite/accept addrs); otherwise dial (#215 B39 suggestion b)
- [x] Loopback gtests: relay → direct with every seq once and in order; release; interop; lost candidate; driver-only; automatic move then relay silence (k3-2, k3-3)

**Exit:** dogfood relay → punched upgrade moves media; relay becomes standby.

## k4 — Media-liveness failover + reconnect (M5)

- [ ] Control-channel heartbeat (~500 ms) per path
- [ ] 1.5 s silence on active → switch to standby (K008)
- [ ] No standby → `Reconnecting` call status, relay re-anchor, 30 s window (K008); UI subtitle (i18n EN + zh-Hans)
- [ ] `peer link lost` no longer tears down while the path set / window allows
- [x] TX-only escalate limited to initial connect — already so: `ShouldEscalateTxOnlyDirect` needs cumulative RX = 0 and fires once per call
- [x] **B44:** a failed connect / escalation no longer tears down a recovered direct path — `CallMediaBridge::FailUnlessDirectRecovered` commits if MediaReady and gives a peer hello mid-handshake a 3 s grace before failing (test `FailedAttemptsKeepTheCallWhenThePeersHelloCompletes`). Escalation is still break-before-make (Detach, then circuit) — k3 makes it make-before-break
- [x] **B30 mitigation:** the offerer treats the answerer's accepted call-media hello (keyed from the invite) as an implicit Accept for a 1:1 call it started whose remote is still invited (`CallSessionWorkflow::ApplyImplicitAccept`, via `CallMediaHost::P2pNoteInboundHello`); the real Accept arriving later is idempotent (test `AnswerersHelloActsAsAcceptWhenTheRelayAcceptIsLate`)
- [ ] Close p2p-av-calls a5 "Reconnect after brief network loss" (cross-link)

**Exit:** killing the active path mid-call → ≤ 2 s gap with standby; recover within window without.

## k5 — Network monitor (M7)

- [ ] `foundation/platform/NetworkMonitor` event API (transport, metered/expensive, change generation)
- [ ] Android `registerDefaultNetworkCallback`; iOS/macOS `NWPathMonitor`; Windows `NotifyIpInterfaceChange` + cost; Linux netlink (fallback poll)
- [ ] Reaction: suspect + keepalive burst + fast evict; reachability re-probe + advertise refresh
- [ ] Active call hook → k4 re-anchor
- [x] Always bind mesh socket dual-stack `[::]` (K010), IPv4 only without OS IPv6 support. Audit: advertise / probe targets come from interfaces, not the bind family; wildcard `::` handled like `0.0.0.0`; pp-cpp-amp maps IPv4 peers both ways. Hard-w5 relays now listen on `[::]` with IPv4-NAT'd peers
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

- [x] Hard-lab CGNAT long-hold stall repro: `pp-call-probe --rx-stall-ms/--watch-ms`, `PP_HARD_NAT_STACK_HOLD_MS` / `_RX_STALL_MS` / `_NETEM_A|B`
- [ ] Hard-lab wave: punch-then-relay-drop, NAT rebind mid-call, short NAT timeout, network flip; netem profiles in CI (1 %/2 % loss must keep 60 s both ways)
- [ ] Promote: CALLS.md (path set, migration, reconnect), WIRE_SCHEMAS (hello migrate, caps.mobility, control ops), MESH.md (link events, hygiene), amp docs/KEEPALIVE.md
- [x] Fix doc drift found in survey: calls CURRENT_STATE "V001–V038", CALLS.md "through V038", H009 "plan only" header (+ media-hop-reachability DESIGN status rows)

## Later horizons

- Opportunistic multipath (duplicate send on two paths for lossy cellular)
- Path quality scoring (RTT/loss from heartbeats) choosing between two healthy paths
- N≥3: apply path sets to SFU legs
