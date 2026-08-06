# Host receive policy (call media)

**Tier:** project  
**ADR:** [V032](DECISIONS.md#v032--media-qos-enforcement-playout-sfu-e2e)  
**Code map:** [CALLS.md](../../docs/architecture/CALLS.md)

Admission, queue, drop, and meter rules for every inbound request a libp2p / messaging host can receive on the call path. Complements wire shapes in [WIRE_SCHEMAS](../../docs/contracts/WIRE_SCHEMAS.md) and mesh N019/N021.

---

## Planes

| Plane | Protocols / types | Owner |
|-------|-------------------|--------|
| Signaling | `call_*` E2E controls (+ HTTP inbox) | `RelayReceivePipeline` → `CallSessionManager` |
| 1:1 media | `/pp-browser/call-media/1.0.0` | `CallMediaDirectService` + bridge |
| Group media | `/pp-browser/media-relay/1.0.0` | `MediaRelayService` |
| Reachability | circuit-relay, dial-back, N025 | enable dials only |

---

## Signaling (`call_*`)

| Request | Admit | Queue / drop | Meter |
|---------|-------|--------------|-------|
| `call_invite` | Always decode; ring only if not same-call Joined; TTL ~60s | Pending invite store; stale TTL expire | — |
| `call_accept` | Joined ≤ eng cap (8); mid-call may refuse without hop | Single active local call (Accept leaves other) | — |
| `call_decline` / `call_leave` / `call_ended` | Session must exist | — | — |
| `call_roster` | Session | May SoftMigrate | — |
| `call_media_key` | Session + unwrap OK | — | — |
| `call_sfu_attach` / `_failed` / `call_hop_refuse` | Topology rules (V028–V030) | Attach-wait | — |
| `call_sdp` / `call_ice` | **Ignore** | — | — |

No call-control-specific rate limit beyond general messaging / HTTP relay limits.

---

## 1:1 media host (`call-media`)

| Request | Admit | Queue / drop | Meter |
|---------|-------|--------------|-------|
| Inbound stream | Reject if session already active; reject while outbound hello in flight (glare) | One duplex | — |
| Hello | Local session + media key (wait ≤8s) | Reject → close | — |
| Audio frame | AEAD under call media key | Decrypt fail → **drop**; **1** in-flight write/peer + latest-wins coalesce | — |

---

## Group media host (`media_relay`)

| Request | Admit | Queue / drop | Meter |
|---------|-------|--------------|-------|
| `quote` | First dialer for `call_id`: contact/scope (V028). Max **4** concurrent `HostSession`s | — | Builds A↑/A↓/B↑/B↓ + ceiling |
| `accept` | Known quote; same admission | — | — |
| `attach` | Auth stub `auth == call_id`; max **8** participants/session; call-scoped strangers OK after session exists | — | Bind per-peer A↑/A↓ |
| subscribe / unsub / detach | Attached participant | — | — |
| Data frame (audio = `ReliableOrdered`) | Attached + subscribed peers | **A↑** on publisher: over budget → drop frame (no fan-out). **A↓** on subscriber: over → skip that peer. Session **ceiling_bytes**: skip all fan-out. Outbound: **one BlockingWrite per peer**; while in flight, **latest-wins coalesce** (no Normal-lane pile-up). **Never** LatestLossy skip-to-latest on audio | `bytes_up` / `bytes_down` / `bytes_total` |
| Data frame (video = `LatestLossy`) | Same | Stale `seq` (mark=0) → drop; same bps/ceiling rules | Same |

Hop stays **blind** (no decode, no call keys).

---

## Client receive (after hop / direct)

| Stage | Policy |
|-------|--------|
| SFU payload | AEAD under call media key (stream-scoped AAD); decrypt fail → drop |
| Opus decode | **Per `stream_id` decoder** (group) |
| Playout | Per-stream jitter target **60 ms**, max **200 ms**; PLC on gap; mix → one SDL device |
| Adaptation | `path_pressure` from playout underrun / hop drops → Opus bitrate within V024 ladder |

---

## Who enforces ↑/↓

| Budget | Enforcer | Action |
|--------|----------|--------|
| **A↑** (per publisher) | Hop on ingress to Fanout; sender via Opus target bps | Drop excess at hop; reduce encode rate |
| **A↓** (per subscriber) | Hop on egress Fanout | Skip that subscriber |
| **B↑ / B↓** | Hop session totals (soft; same drop path when over) | Skip fan-out when session over |
| **Ceiling** | Hop `bytes_total` | Skip fan-out (no bill above ceiling) |
| **Delay** | Receiver playout only | Hop does not buffer for jitter |

---

## Observability (health UI)

| Surface | Who | Source |
|---------|-----|--------|
| Quality bars + Fair/Poor/NoAudio | Everyone | `EvaluateCallMediaHealth` ← engine + hop snapshots |
| Call details sheet | Everyone (thin); debug extras when gated | Same + clipboard copy |
| Debug subtitle / rich numbers | `call_diagnostics` pref **or** `--debug` | `CallDiagnosticsEnabled` |
| `media_health` INFO line (~2s) | Logs | `FormatMediaHealthLogLine` |

---

## Out of scope (this policy)

- Paid pricing / C↑ node capacity auction  
- Congestion-control feedback protocol (RTCP-like) — later  
- Video publish on libp2p (deferred)  
Fork `src/libp2p/fork` internals except via integration services
