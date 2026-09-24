# Call path resilience — current state

**Last updated:** 2026-09-24 (k0 code complete: link events + `MeshLink` log, path-label truth, loopback red test; dogfood root cause pending)

## Landed

| Area | State |
|------|-------|
| Project docs | README / DESIGN / PHASES / DECISIONS (K001–K010) |
| Prerequisite fix | pp-cpp-amp **v2.1.9**: nested-carrier failure drop deferred to Tick (answerer SIGSEGV on carrier reset mid-handshake); pinned in `cmake/PpCppAmp.cmake` |
| **k0 link events** | Amp `LinkEvent` (Connected / Dropped+`LinkDropReason` / PathChanged) via `MeshRuntime::AddLinkEventListener` ([ADR_LINK_PLANE §9](https://github.com/people-post/pp-cpp-amp/blob/develop/docs/ADR_LINK_PLANE.md)); pp-browser `MeshLinkEventLog` logs them (`MeshLink`: connected/path INFO, drop of connected link WARNING, failed attempts DEBUG). pp-cpp-amp **v2.1.10** pinned. |
| **k0 path label** | Call details path = bound link kind (`ActiveLinkKind`); answerer no longer shows "Punched" while on the relay carrier |
| **k0 red test** | `DISABLED_CallSurvivesRelaySilenceWithDirectPath` reproduces the dogfood tail in loopback (relay silent → both legs `peer link lost` although a direct link is up) — the k3/k4 acceptance test |
| Dogfood tooling | `{data_dir}/logs/pp-browser.log`, `crash_pending.txt` `image_base=`, `scripts/dev/pp_dogfood.sh` ([CONFIGURATION.md § Log file](../../docs/ops/CONFIGURATION.md)) |

## Still open

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
