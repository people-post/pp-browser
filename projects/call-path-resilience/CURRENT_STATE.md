# Call path resilience — current state

**Last updated:** 2026-09-25 (one-way stall fix; k1 small hygiene; probe runs product threading → direct-chat ack, lock-order, idempotent Stop fixes)

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

## Still open

- **One-way audio stall on relayed calls — root-caused and fixed (needs pp-node on relays):** the hop bound the dialer's circuit channel Control (Reliable, strict in-order) and never switched it to the carrier policy; the dialer sends best-effort, so the first lost / reordered frame wedged dialer→target for the rest of the call (dogfood 16:17). Reproduced in hard lab CGNAT (`delay 80ms loss 1%` on the caller) and loopback (`RelayedCallDisturbanceTest.CallerUplinkLossDoesNotWedgeCallerToCallee`); fixed in `CircuitTunnelCoordinator` (hop side). Lab after fix: 60 s both ways under 1 % and 2 % loss.
- **Half-open call-media bundle taken as connected** (lab, 1 % loss + glare): InCall with no media, no retry — fixed (only MediaReady counts; `HalfOpenBundleIsNotAConnection`).
- **Nested Reliable channels over a best-effort carrier** (call control, Amp chat, call-media hello over a relay): no end-to-end retransmission — lab `delay 120ms 30ms` (heavy reordering) fails call signaling (`amp direct chat send timed out`). A024 "dual outer lanes" follow-on; see k1.
- ~~`call_leave` lost at hangup in the lab~~ — probe race, not product: `LeaveCall` sends after the UI is Idle and the probe shut chat down first; the answerer (short mode) also hung up on its first RX frame, which the lost leave had masked. Probe now flushes the leave and the answerer waits for the offerer; the smoke gives the answerer 10 s to exit on its own (143 now means "never saw the leave").
- **Caller probe hang / crash after Leave — fixed structurally (2026-09-25).** The probe drove Amp from its main (UI) thread, so any main-thread wait on mesh progress deadlocked, and it tore down in its own order. Now the probe runs the product threading and teardown, which exposed four product bugs, all fixed:
  - **Amp direct chat never acked under MeshPump (product).** Inbound request handlers returned `false` / used `read_once`, closing the channel before the MeshControl worker replied; every direct-chat send timed out after 4 s and fell back to the relay (likely a large part of **B30** signaling latency). All eight worker-answered L4 handlers now reply via `InboundReply` (`l4/shared/InboundReply.h`); dial-back also reads the observed endpoint on IO. Test: `AmpDirectChatMeshPumpTest`.
  - **Lock-order inversion on quit / Leave (product).** Off-IO `AbortInflight` took the coordinator mutex, then the Amp strand; MeshPump holds the strand, then the mutex. Circuit / media-relay `AbortInflight` and call-media `Stop` now enter via `MeshRuntime::WithIoLock` (THREADING.md lock-order rule). The product's 3 s quit watchdog had been hiding this. No unit test (needs a live reservation racing the pump); the hard lab covers it.
  - **Non-idempotent `Stop()` (product).** The destructor's second `Stop` touched a freed runtime and dropped a replacement owner's protocol handler. Fixed for nine L4 classes. Test: `CallMediaLegCoordinatorTest.DestroyAfterStopKeepsReplacementHandler`.
  - **Single stop order:** `CallStack::StopMesh` (hub + probe); probe teardown = abort → quiesce → StopMesh → runtime join → free; `MeshHost::AttachAmpStack(…, AttachDrive::MeshPump)`.
  - The probe watchdog now fails a stuck teardown after 40 s and dumps every thread's backtrace (addr2line on the host), so the lab can't hang forever.
- **Pre-existing flake (not yet investigated):** `CallUiBackendStackTest.*` segfaults under parallel load (`ctest -j8`: about 1 run in 2; 12 of 12 when 12 copies run at once, including on the baseline without these changes). Serial runs pass. Needs ASan.
- **k1 small hygiene — fixed:** inbound dial key hex, ephemeral burst alias on inbound adopt (amp `c36bf10`, **not yet released/pinned**); reach loop "punched"/"direct" on a relay-carrier-only link (`IsConnectedDirect`).

From PR #223 / #215 (dogfood 2026-09-24 evening, phone CN cellular ↔ Mac Wi‑Fi):
- **B30** relay signaling latency on CN cellular (invite/accept 11–58 s late; phone `PollInbox ok=13 failed=97`) — now the dominant failure; relay/infra + client mitigation below (k4).
- **B44** offerer `TxOnlyGraceExpired → CircuitEscalated` raced the answerer's successful redial; the failed escalation tore down the live direct call (k4).
- Answerer waits ~12 s (`await_circuit_ready` / seed park) before its first dial (k2).
- Relay still replays ~10 stale Accept/Invite per poll (B40 only stops them touching the DialBook) — relay-side.

- **10.0 s audio loss after punch** (dogfood 2026-09-24): trigger unexplained; tail consistent with R1 link eviction → carrier orphan → `peer link lost`. Needs k0 link events from caller, answerer and relay.
- All phases k0–k7.

## Next agent — start here

1. Rerun the cross-network dogfood with `scripts/dev/pp_dogfood.sh -- --debug` on both ends plus relay logs — `MeshLink` lines show which link died and why.
2. k0 exit still needs the dogfood answer (the 10 s trigger). Code-wise, next is **k2** (call links hot + reservation refresh) and **k1** (amp hygiene, incl. snapshot fields) in parallel.
3. **k2** mitigation (hot call links + reserve refresh) can start in parallel — smallest change likely to stop the relay path dying mid-call.

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
