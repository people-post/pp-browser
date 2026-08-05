# Network status chrome — phases

**[DESIGN.md](DESIGN.md)** is the authoritative product spec (locked by S003–S010).  
**This file orders work only.**

## s0 — Project + decisions

- [x] README / DESIGN / OPEN_QUESTIONS / DECISIONS / PHASES / CURRENT_STATE
- [x] Index in [projects/README.md](../README.md)
- [x] Resolve blocking Q1–Q7 → ADRs [S003–S009](DECISIONS.md)
- [x] Resolve important Q8–Q17 → ADR [S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)
- [ ] Optional: static HTML mock for cluster + popover (team review)

## s1 — Ambient cluster (display-only)

- [x] Status cluster RML/RCSS in `#shell-statusbar` (A+B; C when help on; D deferred to s3)
- [x] Wire Mesh from host running / error
- [x] Wire Reach from `ReachabilitySnapshot` only ([S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db))
- [x] Wire Help visible when Node / Help the network on (idle vs active with counts → s3)
- [x] Help SVG + semantic colors; i18n keys; settings string parity for reach labels
- [x] Ambient recolor for OutboundOnly/Blocked (independent of nudge ack)
- [x] Update WINDOW_SHELL + design-system inventory
- [x] Unit tests for `BuildStatusbarClusterSnapshot`
- [x] Keep display-only (click in s2)
- [ ] Desktop Node dogfood (Reachable + Helping icon; Outbound/Blocked labels)

## s2 — Click → hybrid popover

- [ ] Hit target on left cluster
- [ ] Anchored popover above bar ([S005](DECISIONS.md#s005--click--hybrid-popover--settings-link-q3-c))
- [ ] Reach summary + Retest; last error; help/load echo ([S006](DECISIONS.md#s006--detail-inspect--retest-no-capability-toggles-q4-b))
- [ ] “Open Network settings…” deep-link to Me → Network
- [ ] No capability toggles in popover
- [ ] Accessible names for icon-only healthy states

## s3 — Relay runtime stats (counts MVP)

- [ ] `RelayRuntimeStats` (name TBD) snapshot from circuit + media services
- [ ] Active counts in bar Load slot + popover ([S008](DECISIONS.md#s008--load-mvp-is-active-counts-only-q6-a), [S009](DECISIONS.md#s009--helper-privacy-aggregates-only-q7-a))
- [ ] Hide zeros; aggregates only (no peer identities)
- [ ] (Later / post-MVP) windowed throughput, then delay/RTT — detail first

## s4 — Polish & harden

- [ ] Truncation / width budget under EN + zh-Hans
- [ ] Transitional motion only ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17))
- [ ] Dogfood: Node helper + one client; Retest from popover; counts under load
- [ ] Promote normative bits to `docs/ui/`; freeze ADRs
