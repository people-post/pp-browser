# Network status chrome — phases

**[DESIGN.md](DESIGN.md)** is the draft spec. **This file orders work only.**  
**Gate:** Do not start **s1** until blocking questions in [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) are resolved (or explicitly deferred via ADR).

## s0 — Project + decisions

- [x] README / DESIGN / OPEN_QUESTIONS / DECISIONS / PHASES / CURRENT_STATE
- [x] Index in [projects/README.md](../README.md)
- [ ] Resolve blocking Q1–Q7 → ADRs S003–S009
- [ ] Resolve important Q8–Q17 as needed for s1–s2
- [ ] Optional: static HTML mock for cluster + detail (team review), similar spirit to shell layout review

## s1 — Ambient cluster (no fake metrics)

- [ ] Status cluster RML/RCSS in `#shell-statusbar` (slots A–C; D placeholder if gated)
- [ ] Wire Mesh from host running / error
- [ ] Wire Reach from `ReachabilitySnapshot` (+ relay sub-signal per S007)
- [ ] Wire Help on/off (/ idle vs active if counts exist)
- [ ] Icons + semantic colors; i18n keys
- [ ] Update WINDOW_SHELL + design-system inventory
- [ ] Still display-only **unless** Q3 says click lands in same phase

## s2 — Click → detail

- [ ] Hit target on left cluster
- [ ] Detail primitive per S005 (popover / deep-link / hybrid / sheet)
- [ ] Reuse reachability summary + Retest per S006
- [ ] Link into Me → Network
- [ ] Coexist with Me attention nudge vocabulary

## s3 — Relay runtime stats

- [ ] `RelayRuntimeStats` (name TBD) snapshot from circuit + media services
- [ ] Active counts in bar Load slot + detail tables
- [ ] Windowed throughput (if S008 includes rates)
- [ ] Delay/RTT (if S008 includes delay)
- [ ] Privacy rules per S009
- [ ] Graceful empty/zero hiding

## s4 — Polish & harden

- [ ] Truncation / width budget under real i18n (EN + zh-Hans)
- [ ] Motion budget (Q15)
- [ ] Accessibility names for icon-only states (Q12)
- [ ] Dogfood gate (Q17)
- [ ] Promote normative bits to `docs/ui/`; freeze ADRs
