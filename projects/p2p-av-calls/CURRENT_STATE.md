# P2P A/V calls — current state

**Last updated:** 2026-08-06

**North star:** [NETWORKING.md](../../docs/architecture/NETWORKING.md) + **[V026](DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking)** — HTTP + libp2p only; call media on libp2p (voice-first). **m2 done:** libdatachannel removed from build; wire-compat `call_sdp`/`call_ice` ignored.

Dogfood / codebase board for **this week**. Stable code map: [docs/architecture/CALLS.md](../../docs/architecture/CALLS.md) (**Call lifecycle**). Product rules: [DESIGN.md](DESIGN.md) / [DECISIONS.md](DECISIONS.md).

## Landed

| Area | State |
|------|-------|
| Project docs | a3 done; **a4 thin**; **V026** libp2p-only media |
| ADRs | V001–**V027** |
| a2/a3 media | Historical LAN WebRTC dogfood (a2–a3); **not** product path after m2 |
| **a4 thin** | Soft-migrate to `media_relay` when N≥3 |
| Hop reachability | Program in [media-hop-reachability](../media-hop-reachability/) — **in-libp2p** (L1+); app `call_hop_addrs` **not** product |
| **CallLifecycle orchestrator** | Phase machine owns ring/accept/media/listen desire; thin `CallController`; N025 from `WantEphemeralListen`; bridge reports MediaDeferred / DirectConnected / ConnectFailed |
| **m1 mobile LAN voice** | Android ↔ Android 1:1 Opus on `/pp-browser/call-media/1.0.0` — **dogfood OK 2026-08-02** |
| **V031 call chrome modes** | Expanded / Immersive / Minimized + gestures landed (people grid for group voice; minimize chip) |
| **V032 media QoS structure** | Host receive policy doc; hop A↑/A↓ token buckets + session/participant caps; per-`stream_id` Opus + jitter playout; path_pressure → Opus bps; SFU AEAD under call media key |
| **Call media health UI** | Quality bars + Fair/Poor/NoAudio labels on call chrome; Call details sheet; debug subtitle + rich diagnostics behind `call_diagnostics` pref / `--debug`; periodic `media_health` INFO logs |

## a4 thin in code (still relevant under V026)

| Area | State |
|------|-------|
| Topology | N≥3 → sticky initiator `RankMediaHops` → quote/attach → `call_sfu_attach` |
| Hop pick | Contacts ∪ org seed via `MeshHopPolicy`; PreferInCall; needs dialable **multiaddr** until L1 peerstore |
| Budgets / framing | N019 / N021 on SFU path |
| **m2 teardown** | libdatachannel unlinked; `CallP2pSignalingBridge` deleted; libp2p-only 1:1 |

## m1 mobile LAN — dogfood claimed (2026-08-02)

**Devices:** moto g7 play (`ZY323QRNJ9`) + Samsung SM-T380 (`dc07955772d54e6c`), same Wi‑Fi; package `dev.pp_browser.app`.

**Path:** Invite-embedded MediaKey → N025 ephemeral listen → answerer reverse-dial (primary) / offerer dial after inbound grace if answerer dialable (asymmetric LAN) → hello/ack → `DirectConnected` / `InCall` → bidirectional Opus (AEAD under call media key).

**Matrix:**

- [x] Accept click → `Accepting` / `AcceptInvite` off Browser IO
- [x] N025 `WantEphemeralListen` + bound `/tcp/<nonzero>`
- [x] `DirectConnected` / `Call-media Connect ok` → `InCall`
- [x] Bidirectional voice (no connect banner; audible both ways)
- [x] Leave → `Idle` (process stays up)

Filter: `adb logcat -s pp-browser:W` — release emit floor promotes INFO→WARNING, so lifecycle / call-media / ephemeral-listen **info** traces still appear.

**Implementation notes (call-media):**

- Answerer reverse-dials first; offerer waits ~8s for inbound then falls back to dial if the answerer is reachable (asymmetric LAN) — still one `newStream` at a time (`keep_inbound` if the other side wins the race)
- Capture enqueues frames; **host IO thread** owns Yamux read/write (async pump) — do not block `read`/`write` on a worker while IO delivers
- Yamux `WriteQueue` copies on enqueue; `ReadBuffer::consumePart` soft-fails bad offsets (see [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md))
- Keep Accept / MediaKey-send / Connect / Poll HTTP **off** Browser IO

## Still open

| Area | State |
|------|-------|
| **m1** desktop matrix | Android ↔ desktop voice without WebRTC; **Windows LAN mDNS** + **call-control `listen_multiaddrs`** on invite/accept for dial when mDNS misses — rebuild **both** ends |
| Hop peerstore / circuit PeerId dial | media-hop **L1–L3** |
| **Transport session SMs (V033 / N026)** | **s2a + s3a + s3b** — call-media phases + Fail-after-Detach ignore; media-relay inbound + client attach phases; Detach aborts AcceptAndAttach; loopback tests green; Android / SoftMigrate dogfood gates open |
| Video on libp2p | Deferred |
| Group SoftMigrate in lifecycle | Phase hook reserved; not v1 |
| N≥3 unify engine on libp2p send/recv | N021 follow-on |

## Next agent — start here

1. **V033 s2b:** Android↔Android dogfood with `phase=` / `event=` logs (`adb logcat -s pp-browser:W`).
2. **m1** finish desktop dogfood (Android ↔ desktop libp2p voice).
3. **media-hop L1:** peer address book in vendored libp2p / `PeerSessionManager`.  
4. Mesh [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup); confirm seed `media_relay`.  

## Agent traps

| Wrong | Right |
|-------|-------|
| Reintroduce `call_hop_addrs` / app ICE gather | H007 — reachability **in** libp2p |
| Extend libdatachannel for 1:1 | Removed in m2 — libp2p media only |
| SoftMigrate invents NAT | Stack dialable? then quote |
| Invent N025 listen from `TopPendingInvite` on tick | Lifecycle `WantEphemeralListen` only |
| Full-shell `SyncLayout` for Accept chrome | `RemountCallChrome` into `#shell-call-*-mount` only |
| Host-wide inbound request SM / rewrite working call-media “while here” | V033 — targeted session SMs; [SESSION_MACHINES.md](SESSION_MACHINES.md) docs first |
| Move `CallLifecycle` phases into `integration/host` | Product SM stays in feature; transport SM in host |
| Always-mounted `data-if` + Dirty for Accept layer | Presence mount via `RemountCallChrome`; Dirty only for labels/pulse inside a mounted layer |
| Recreate `CallLibp2pMediaBridge` on N025 sync | Only when `CallSessionManager*` changes |
| Call-media `read`/`write` from a non-IO worker while pump runs | `Libp2pHost::Post` async pump only |
| Hold a mutex across blocking stream read from capture | Enqueue + IO-thread write |
| Put Accept / Connect / PollInbox on Browser IO | Dedicated workers / hop off IO |
