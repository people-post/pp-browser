# Call path resilience — current state

**Last updated:** 2026-09-28 (k6: mobility + pair policy + relay standby; k5: network monitor + Amp network-change probing + lab FLIP; k7: punchable lab NAT, upgrade / failover / punch lab phases, quiet rebind; earlier: B30, B44, seed-park grace, dual-stack bind, k1 Amp hygiene v2.4.0, nested reliable lane v2.5.0, k3, k4)

> **Code moved since 2026-09-25.** Calls run on the media-sessions owner (`CallsThread`), reach / rendezvous / mesh media plane / reachability on the Connectivity owner, MeshControl is gone (mesh waits are completions), the inbound call-media hello is asynchronous, and pp-cpp-amp is pinned at **v2.6.0**. Mentions of MeshControl, "UI thread" bridge state, or amp v2.2.x below are history. Product hot keepalive is **10 s** (K008 amendment), not 2 s.

## Landed

| Area | State |
|------|-------|
| Project docs | README / DESIGN / PHASES / DECISIONS (K001–K010) |
| Prerequisite fix | pp-cpp-amp **v2.1.9**: nested-carrier failure drop deferred to Tick (answerer SIGSEGV on carrier reset mid-handshake); pinned in `cmake/PpCppAmp.cmake` |
| **k0 link events** | Amp `LinkEvent` (Connected / Dropped+`LinkDropReason` / PathChanged) via `MeshRuntime::AddLinkEventListener` ([ADR_LINK_PLANE §9](https://github.com/people-post/pp-cpp-amp/blob/develop/docs/ADR_LINK_PLANE.md)); pp-browser `MeshLinkEventLog` logs them (`MeshLink`: connected/path INFO, drop of connected link WARNING, failed attempts DEBUG). pp-cpp-amp **v2.1.10** pinned. |
| **k0 path label** | Call details path = bound link kind (`ActiveLinkKind`); answerer no longer shows "Punched" while on the relay carrier |
| **k0 red test** | `DISABLED_CallSurvivesRelaySilenceWithDirectPath` reproduces the dogfood tail in loopback (relay silent → both legs `peer link lost` although a direct link is up) — the k3/k4 acceptance test |
| **k1 keepalive v2** | pp-cpp-amp **v2.2.0** (pinned): keepalive announces cadence + echo, window max(5 s, 5/2 × cadence), warm/hot dead-peer eviction, pending tiers; product hot 10 s / warm 25 s. **Wire change — relays/seeds need the new pp-node.** |
| **k2 relay reservations** | Dogfood 10:50 "Couldn't connect": reserved relay links evicted as `connection-dead` 8–11 s after park (cold link; peer's 5 s liveness) and the 15 s lease was never renewed. Now: relay link hot while reserved, product hot keepalive 2 s, reservations renewed every 10 s while a media session lives. |
| Shutdown crash (not a k-phase) | Dogfood 10:51 SIGSEGV (relay Send during teardown) + audit of ~15 owners → central `AppRuntime` teardown quiesce ([THREADING.md § Teardown quiesce](../../docs/architecture/THREADING.md#teardown-quiesce)); quit and profile reset quiesce before freeing messaging; profile reset no longer leaves mesh refusing to start. |
| **PR #223 (B39–B43)** merged | Cross-network reconnect fixes (dogfood 21:33–21:47: 3/4 calls connected, was 0/2): B40 live-session-only accept addrs + no link-local/loopback advertised; B41 self excluded from rendezvous; B42 watchdog fails one attempt (V049 re-dial runs); B43 coordinator lost wake-up; B39 bridge drops a stale "connected" link after a failed attempt and forces a redial — now via amp `RequestDropLink` (reason `requested`, amp `cfaddc2`) instead of `FindLink` + `Connection::Close` (pp-cpp-amp **v2.2.1**, pinned). |
| Dogfood tooling | `{data_dir}/logs/pp-browser.log`, `crash_pending.txt` `image_base=`, `scripts/dev/pp_dogfood.sh` ([CONFIGURATION.md § Log file](../../docs/ops/CONFIGURATION.md)) |

## Landed 2026-09-27

| Item | State |
|------|-------|
| **B30** implicit Accept | Offerer treats the answerer's accepted call-media hello as the accept (1:1, remote still invited); the late relay Accept is idempotent |
| **B44** | A failed connect / escalation commits a recovered direct path, and gives a peer hello mid-handshake a 3 s grace, instead of `SurfaceConnectFailed` |
| **Answerer 12 s wait** | Seed park settles 2 s after the first Connected seed (was: the full 12 s whenever one seed was unreachable) |
| **K010** dual-stack | Mesh socket always `[::]` (IPv4 fallback only without OS IPv6) |
| `CallUiBackendStackTest` parallel flake | Gone — 0 / 60 with 12 concurrent copies (was 12 / 12); fixed by thread-ownership's port snapshots |
| Doc drift (k7) | V049 range, H009 status, media-hop-reachability status rows |
| **k1 Amp hygiene** (pp-cpp-amp **v2.4.0**, pinned) | Carrier-closed and failed-inbound links dropped; only fresh packets move the path or prove liveness; drops by `LinkHandle` (A024 key sharing); nested and ADP establishes never wait on each other; OS-unreachable sends drop the link at once; `idle_ttl` removed; snapshots / events carry `LinkPathKind`, remote, RX age ([ADR_LINK_PLANE §10](https://github.com/people-post/pp-cpp-amp/blob/develop/docs/ADR_LINK_PLANE.md)). pp-browser full suite, TSan (mesh + calls) and hard-w5 green against it. `MeshLinkEventLog` prints `path=direct|punched|carrier` |
| **k3 make-before-break migration** (k3-0 … k3-4) | A live call moves between links to one peer without dropping: `migrate` / `migrate_ack` / `path_release` on the new link, TX switch, old path released after RX on the new one ([AMP-CHANNEL.md § Path migration](../../docs/contracts/AMP-CHANNEL.md)). The transport takes a direct link for a relayed call by itself; the offerer punches for one (+3 / +20 / +60 s). A second hello no longer kills a live call. A TX-only call (no frames arriving — not no audio) moves onto a circuit under it instead of restarting; either end may migrate (glare: offerer wins). Open: B39 b |
| **k4 failover + reconnect** (k4-1 … k4-3) | Per-path heartbeat (500 ms active / 10 s standby); a released path stays as warm standby; active link lost or 1.5 s silent (from a heartbeating peer — never a muted mic) → TX onto the standby, the peer follows. No path left → `Reconnecting…` for 30 s while the offerer re-anchors and migrates the call onto the new link; then it fails ([AMP-CHANNEL.md § Path liveness and failover](../../docs/contracts/AMP-CHANNEL.md)). Standby is only what a migration left behind — no proactive relay standby yet (K003, k6) |
| **k1 nested reliable lane** (pp-cpp-amp **v2.5.0**, pinned) | Reliable-class frames on a nested (relay-carrier) link are sequenced end to end, acked, resent and released in order (`CarrierLane`, [ADR_LINK_PLANE §11](https://github.com/people-post/pp-cpp-amp/blob/develop/docs/ADR_LINK_PLANE.md)); a dead end-to-end path drops the nested link. Probe-negotiated — older peers and relays unchanged. Amp gtest: 20 % loss + reordering both ways delivers every message in order (red before). Lab `delay 120ms 30ms` stack / cold pass — but also passed on v2.4.0 in a single cycle, so the lab does not discriminate yet (k7 netem wave) |

## Landed 2026-09-28 (k7)

| Item | State |
|------|-------|
| **Lab NAT was un-punchable by accident** | The CGNAT gateways accepted unsolicited WAN input, so a peer's early punch packet became a conntrack flow and MASQUERADE remapped our own outbound port — every lab punch failed. Gateways now drop it (as routers do) and run an explicit mapping: symmetric (default; phases 1–8 stay relay-shaped) or cone ([HL005](../hard-lab/DECISIONS.md)) |
| **Introducer observed endpoints** | The relay introducing a punch leads each side's candidates with the endpoint it sees for the peer; self-reported candidates were private-only in the lab (and behind any NAT without a learned public address) |
| **hard-w5 Phase-9 UPGRADE / Phase-10 PUNCH** | Relayed → direct by the +20 s upgrade punch on both ends → direct blackholed → failover to the relayed standby; call-start punch with media on the punched link and no `Reconnecting`. Full hard-w5 green; PUNCH 14/14 in a loop |
| **Quiet rebind (K011)** | A lost path while the peer is Connected on another link (dual-dial election after a simultaneous punch) → the offerer migrates at once; `Reconnecting…` only if not landed in 1 s |
| **Hello-born bundle role** | The offerer joining a bundle the answerer's early hello created kept "answerer"; with the PeerId against it nobody drove (lab: 2 / 11 PUNCH runs showed `Reconnecting`) |
| **Reconnect placeholder** | Was retired into a channel-less "standby" a later failover could switch onto — now dropped |
| **Channel close off the IO strand** | A media send hitting a dead channel ran close handling on the sender's thread: coordinator `mu` → link manager lock, the reverse of the IO pump (TSan). Close handling is posted to IO |

## Landed 2026-09-28 (k5)

| Item | State |
|------|-------|
| **Amp network change** (pp-cpp-amp **v2.7.0**, pinned) | `NotifyNetworkChanged`: every direct link probed at once, silent ones dropped after 2 s (`network-changed`) instead of their liveness window (up to 50 s hot); dial backoffs cleared |
| **NetworkMonitor** | `foundation/platform`: Linux rtnetlink, Darwin `NWPathMonitor`, Windows IP helper + cost hint, Android default-network callback. Only Linux is exercised here (live check in a network namespace); the others build in CI — **device check pending** |
| **Reaction** ([K012](DECISIONS.md)) | Mesh: probe links + re-probe reachability (advertised / punch addrs). Calls: reconnecting → re-anchor after 2.5 s; relayed → upgrade punches start over. Offline → nothing |
| **Lab FLIP** | peer-a changes address mid-call: new path 2.3 s after the flip, 6 / 6 |

## Landed 2026-09-28 (k6)

| Item | State |
|------|-------|
| **Mobility class** | `MobilityClassifier` in `CallStack` from the NetworkMonitor (cellular / metered / churn) and observed-address churn; `caps.mobility` on invite / accept, `call_caps_update` on a flip; override `mesh.mobility` / `--mobility=` |
| **Pair policy** ([K013](DECISIONS.md)) | Mobile pair: no call-start punch, no upgrade, relay anchor (lab Phase-12 MOBILE). Stationary / unknown: punch, upgrade, relay standby |
| **Relay standby (K003)** | A direct / punched call gets a relayed standby (`path_add`) from its offerer; lab FLIP now fails over in 1.5 s, never Reconnecting |
| **Relay admission** | `standby_priority` on bridge requests; relays refuse the least needed standby first, per-dialer cap 4 |
| **Fixes found on the way** | `exclude_direct` reach settled on the direct link (TX-only escalation shared it); Amp nested establish skipped the nested link beside an ADP one — pp-cpp-amp **v2.7.1**, pinned; a path dying before the previous one's release now falls back onto it (and the winning direct link is taken at once); inbound-placeholder use-after-free (keys collided across links) |

## Still open (as of 2026-09-24/25 — see the note above)

- **One-way audio stall on relayed calls — root-caused and fixed (needs pp-node on relays):** the hop bound the dialer's circuit channel Control (Reliable, strict in-order) and never switched it to the carrier policy; the dialer sends best-effort, so the first lost / reordered frame wedged dialer→target for the rest of the call (dogfood 16:17). Reproduced in hard lab CGNAT (`delay 80ms loss 1%` on the caller) and loopback (`RelayedCallDisturbanceTest.CallerUplinkLossDoesNotWedgeCallerToCallee`); fixed in `CircuitTunnelCoordinator` (hop side). Lab after fix: 60 s both ways under 1 % and 2 % loss.
- **Half-open call-media bundle taken as connected** (lab, 1 % loss + glare): InCall with no media, no retry — fixed (only MediaReady counts; `HalfOpenBundleIsNotAConnection`).
- ~~**Nested Reliable channels over a best-effort carrier**~~ (call control, Amp chat, call-media hello over a relay): no end-to-end retransmission — fixed by the amp v2.5.0 reliable lane (k1).
- ~~`call_leave` lost at hangup in the lab~~ — probe race, not product: `LeaveCall` sends after the UI is Idle and the probe shut chat down first; the answerer (short mode) also hung up on its first RX frame, which the lost leave had masked. Probe now flushes the leave and the answerer waits for the offerer; the smoke gives the answerer 10 s to exit on its own (143 now means "never saw the leave").
- **Caller probe hang / crash after Leave — fixed structurally (2026-09-25).** The probe drove Amp from its main (UI) thread, so any main-thread wait on mesh progress deadlocked, and it tore down in its own order. Now the probe runs the product threading and teardown, which exposed four product bugs, all fixed:
  - **Amp direct chat never acked under MeshPump (product).** Inbound request handlers returned `false` / used `read_once`, closing the channel before the MeshControl worker replied; every direct-chat send timed out after 4 s and fell back to the relay (likely a large part of **B30** signaling latency). All eight worker-answered L4 handlers now reply via `InboundReply` (`l4/shared/InboundReply.h`); dial-back also reads the observed endpoint on IO. Test: `AmpDirectChatMeshPumpTest`.
  - **Lock-order inversion on quit / Leave (product).** Off-IO `AbortInflight` took the coordinator mutex, then the Amp strand; MeshPump holds the strand, then the mutex. Circuit / media-relay `AbortInflight` and call-media `Stop` now enter via `MeshRuntime::WithIoLock` (THREADING.md lock-order rule). The product's 3 s quit watchdog had been hiding this. No unit test (needs a live reservation racing the pump); the hard lab covers it.
  - **Non-idempotent `Stop()` (product).** The destructor's second `Stop` touched a freed runtime and dropped a replacement owner's protocol handler. Fixed for nine L4 classes. Test: `CallMediaLegCoordinatorTest.DestroyAfterStopKeepsReplacementHandler`.
  - **Single stop order:** `CallStack::StopMesh` (hub + probe); probe teardown = abort → quiesce → StopMesh → runtime join → free; `MeshHost::AttachAmpStack(…, AttachDrive::MeshPump)`.
  - The probe watchdog now fails a stuck teardown after 40 s and dumps every thread's backtrace (addr2line on the host), so the lab can't hang forever.
- **Pre-existing flake (not yet investigated):** `CallUiBackendStackTest.*` segfaults under parallel load (`ctest -j8`: about 1 run in 2; 12 of 12 when 12 copies run at once, including on the baseline without these changes). Serial runs pass. Needs ASan.
- **k1 small hygiene — fixed:** inbound dial key hex, ephemeral burst alias on inbound adopt (pp-cpp-amp **v2.2.2**, pinned); reach loop "punched"/"direct" on a relay-carrier-only link (`IsConnectedDirect`).

From PR #223 / #215 (dogfood 2026-09-24 evening, phone CN cellular ↔ Mac Wi‑Fi):
- **B30** relay signaling latency on CN cellular (invite/accept 11–58 s late; phone `PollInbox ok=13 failed=97`) — now the dominant failure; relay/infra + client mitigation below (k4).
- **B44** offerer `TxOnlyGraceExpired → CircuitEscalated` raced the answerer's successful redial; the failed escalation tore down the live direct call (k4).
- Answerer waits ~12 s (`await_circuit_ready` / seed park) before its first dial (k2).
- Relay still replays ~10 stale Accept/Invite per poll (B40 only stops them touching the DialBook) — relay-side.

- **10.0 s audio loss after punch** (dogfood 2026-09-24): trigger unexplained; tail consistent with R1 link eviction → carrier orphan → `peer link lost`. Needs k0 link events from caller, answerer and relay.
- All phases k0–k7.

## Next agent — start here

1. **k1, k3, k4 done; k7 largely done** (lab wave remainder below). Path set, migration, failover, reconnect and quiet rebind are summarized in [CALLS.md § Call media paths](../../docs/architecture/CALLS.md#call-media-paths-11--call-path-resilience); wire in [AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md).
2. **k2** remainder: the call-scoped per-link keepalive interval (the product picks the value first).
3. **k5, k6 done** except device checks (Wi-Fi ↔ cellular / sleep-wake on each platform; mobility classes on real phones). Later: opportunistic upgrade for unmetered mobile pairs (K013), user "Connection preference". Lab: short NAT timeout still open.
4. Open: B39 suggestion b; netem profiles in CI.
5. Dogfood (k0 exit): rerun cross-network with `scripts/dev/pp_dogfood.sh -- --debug` on both ends plus relay logs — the punch now has lab coverage, the real NATs are the next check.

## Agent traps

| Wrong | Right |
|-------|-------|
| Close / cancel the circuit when a punch succeeds | Migrate first (K002); relay becomes standby (K003) |
| Assume a standby relay always exists | Standby is best-effort — relay may refuse; handle "no standby" via re-anchor (K003) |
| Treat `ConnectSucceeded` while Live as success of a new path | It is a no-op "keep" today — media never moved |
| Trust the "Punched" label on the answerer | It can be "Punched" while media is on the circuit (k0 fix) |
| Bump `caps.v` for `caps.mobility` | Optional key only (K005) — `v` bump zeroes caps on old peers |
| New protocol id for migration | Hello type / control ops (K006) |
| `MarkWarm` before `EnsureAssociation` | No-op until k1 remembers pending tiers |
| `DropLink` inside a link / carrier callback | `ScheduleDropLink` (see amp v2.1.9 fix) |
