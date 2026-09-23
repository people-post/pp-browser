# Cross-network dogfood B25–B31 (PR #214)

**Source:** [PR #214](https://github.com/people-post/pp-browser/pull/214) (Kenneth, 2026-09-23) — draft test notes + experimental env knobs.  
**Test matrix:** phone on CN cellular ↔ Mac home Wi‑Fi (IPv6 on); develop `5acdb51e3`, amp v2.1.8 / ui v0.3.1.

First stable cross-network two-way audio observed (**Mac → phone**, IPv6 direct). Reverse direction failed for signaling latency + dial asymmetry.

| Id | Summary | Owner | Status (2026-09-23) |
|----|---------|-------|---------------------|
| **B25** | Warm Connected link with closed ADP never evicted → `send on closed connection` | pp-cpp-amp | **Fixed** in amp (`PeerLinkManager` evicts `conn->IsClosed()` regardless of warm tier); in v2.1.8 pin |
| **B26** | No public IPv4 candidate (LAN-only advertise) → cross-net punch has nothing useful | design / mesh | **Partial** — refresh advertised/punch addrs on reachability (`ec4e647d5`). Still missing STUN-like reflexive learn from seeds’ view of the association |
| **B27** | Circuit `target peer endpoint not registered`; stuck on one relay | circuit + relay | **Partial** — same-relay not-reg once then advance (`CircuitShouldRetrySameRelayOnNotReg`). Needs relay-side visibility (what reserve registers vs tunnel lookup key after network change) |
| **B28** | Wrong one of many peer IPv6 addrs dialed (last-write-wins) | DialBook / reachability | **Open** — `PP_BROWSER_PREFER_PREFIX` is a dev pin only; real fix is per-peer candidate list + short-timeout probing (happy eyeballs) |
| **B29** | Punch needs circuit introducer; direct IPv6 ICMP≠UDP | punch + relay | **Partial** — cold punch tries next introducer; still blocked when all relays not-reg (B27). Longer-term: punch driven off signaling when no circuit |
| **B30** | CN cellular → CloudFront relay inbox polls time out; serial 30 s HTTP stalls signaling | relay ops + client | **Open** — client knobs `PP_BROWSER_HTTP_TIMEOUT_S` / `PP_BROWSER_CALL_INVITE_TTL_MS` (defaults unchanged). Needs reachable relay endpoint and/or long-poll/push |
| **B31** | Answerer one dial + offerer 15 s grace → only Mac-as-caller works under stateful NAT | call-media | **Addressed in client** — [V049](DECISIONS.md#v049--simultaneous-dial-on-accept-cross-nat-open): both dial immediately; re-dial within budget |

## Decisions still needed

1. **B30 relay path (infra / protocol)**  
   - Keep CloudFront + shorten client poll timeout by default?  
   - Or add a CN-reachable relay endpoint?  
   - Or replace serial short-poll with long-poll / push?  
   Client knobs alone do not fix 80 s receive blackouts.

2. **B26 reflexive IPv4**  
   Learn public IP:port from seeds (STUN-shaped) vs require UPnP / global IPv6 only.

3. **B28 candidate probing**  
   Promote DialBook from single Preferred to probed candidate set (scope, timeouts, interaction with punch burst).

4. **B27 / B29 relay registration**  
   Confirm reserve key vs tunnel lookup after peer network change; whether punch may use inbox signaling as introducer without circuit.

## Client knobs (PR #214 commit)

| Env | Default | Experiment | Effect |
|-----|---------|------------|--------|
| `PP_BROWSER_HTTP_TIMEOUT_S` | 30 | 8 | `HttpClient` `CURLOPT_TIMEOUT` (all shared HTTP, including inbox polls) |
| `PP_BROWSER_CALL_INVITE_TTL_MS` | 60000 | 120000 | Ring / outbound unanswered TTL |

Defaults stay production-safe until B30 default policy is decided.
