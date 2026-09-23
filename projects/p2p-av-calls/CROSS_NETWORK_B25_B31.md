# Cross-network dogfood B25–B31 (PR #214)

**Source:** [PR #214](https://github.com/people-post/pp-browser/pull/214) (Kenneth, 2026-09-23).  
**Client follow-up:** [PR #215](https://github.com/people-post/pp-browser/pull/215).  
**Test matrix:** phone on CN cellular ↔ Mac home Wi‑Fi; develop `5acdb51e3`, amp v2.1.8 / ui v0.3.1.

| Id | Summary | Status |
|----|---------|--------|
| **B25** | Warm closed ADP never evicted | **Fixed** (amp v2.1.8) |
| **B26** | No public IPv4 candidate | **Landed (client):** seed dial-back returns `observed` reflexive Amp endpoint; advertise/punch merge requires usable public IPv4/IPv6 when present |
| **B27** | Circuit not-reg / stuck on one relay | **Partial** — client advances relay after one same-relay not-reg; relay keying still needs ops visibility |
| **B28** | Wrong IPv6 of many dialed | **Landed (client):** ingest via Amp `RegisterEndpoints` (best-first); DialBook short-timeout `AdvanceDialCandidate` on miss |
| **B29** | Punch needs circuit introducer | **Scoped:** [H012](../media-hop-reachability/DECISIONS.md#h012--punch-via-call-signaling-when-no-amp-introducer) punch-via-signaling when Amp I missing (L3.25d); Amp introducer path remains primary |
| **B30** | CN cellular relay poll stalls | **Open** (infra / long-poll / push). Env knobs **removed** — not product policy |
| **B31** | Asymmetric dial under stateful NAT | **Landed:** [V049](DECISIONS.md#v049--simultaneous-dial-on-accept-cross-nat-open) |

## Decisions (2026-09-23)

1. **No env knobs** for HTTP timeout / invite TTL — coded policy only; B30 needs relay path design, not dial-a-number experiments.
2. **IPv4 required** — seed-observed reflexive public IPv4 is part of advertise/punch (B26), not IPv6-only.
3. **No hacks** — `PP_BROWSER_PREFER_PREFIX` removed; B28 = real candidate probing.
4. **Punch via signaling** — H012 scopes ACP over call-control when Amp introducer is unavailable (in addition to fixing B27).

## Still open (next work)

| Item | Next |
|------|------|
| **B30** | CN-reachable relay and/or long-poll/push; optional poll-specific fail-fast (not global curl env) |
| **B27** | Relay reserve vs ServeDial lookup after network change |
| **B29 / L3.25d** | Implement `call_punch_*` ↔ `AmpPunchCoordinator` burst per H012 |
