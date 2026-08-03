# P2P A/V calls — current state

**Last updated:** 2026-08-02

**North star:** [NETWORKING.md](../../docs/architecture/NETWORKING.md) + **[V026](DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking)** — HTTP + libp2p only; call media on libp2p (voice-first). WebRTC/libdatachannel = **legacy in tree**.

Dogfood / codebase board for **this week**. Stable code map: [docs/architecture/CALLS.md](../../docs/architecture/CALLS.md) (**Call lifecycle**). Product rules: [DESIGN.md](DESIGN.md) / [DECISIONS.md](DECISIONS.md).

## Landed

| Area | State |
|------|-------|
| Project docs | a3 done; **a4 thin**; **V026** libp2p-only media |
| ADRs | V001–**V027** |
| a2/a3 media | Historical LAN WebRTC dogfood — **not** ongoing path |
| **a4 thin** | Soft-migrate to `media_relay` when N≥3 |
| Hop reachability | Program in [media-hop-reachability](../media-hop-reachability/) — **in-libp2p** (L1+); app `call_hop_addrs` **not** product |
| **CallLifecycle orchestrator** | Phase machine owns ring/accept/media/listen desire; thin `CallController`; N025 from `WantEphemeralListen`; bridge reports MediaDeferred / DirectConnected / ConnectFailed |
| **m1 mobile LAN voice** | Android ↔ Android 1:1 Opus on `/pp-browser/call-media/1.0.0` — **dogfood OK 2026-08-02** |

## a4 thin in code (still relevant under V026)

| Area | State |
|------|-------|
| Topology | N≥3 → sticky initiator `RankMediaHops` → quote/attach → `call_sfu_attach` |
| Hop pick | Contacts ∪ org seed via `MeshHopPolicy`; PreferInCall; needs dialable **multiaddr** until L1 peerstore |
| Budgets / framing | N019 / N021 on SFU path |
| Legacy 1:1 | PeerConnection + ICE until m2 teardown (not used when libp2p media connects) |

## m1 mobile LAN — dogfood claimed (2026-08-02)

**Devices:** moto g7 play (`ZY323QRNJ9`) + Samsung SM-T380 (`dc07955772d54e6c`), same Wi‑Fi; package `dev.pp_browser.app`.

**Path:** Invite-embedded MediaKey → N025 ephemeral listen → answerer-only `call-media` dial → hello/ack → `DirectConnected` / `InCall` → bidirectional Opus (AEAD under call media key).

**Matrix:**

- [x] Accept click → `Accepting` / `AcceptInvite` off Browser IO
- [x] N025 `WantEphemeralListen` + bound `/tcp/<nonzero>`
- [x] `DirectConnected` / `Call-media Connect ok` → `InCall`
- [x] Bidirectional voice (no connect banner; audible both ways)
- [x] Leave → `Idle` (process stays up)

Filter: `adb logcat -s pp-browser:W` — release emit floor promotes INFO→WARNING, so lifecycle / call-media / ephemeral-listen **info** traces still appear.

**Implementation notes (call-media):**

- Answerer dials; offerer keeps inbound stream (`keep_inbound`) — no simultaneous `newStream`
- Capture enqueues frames; **host IO thread** owns Yamux read/write (async pump) — do not block `read`/`write` on a worker while IO delivers
- Yamux `WriteQueue` copies on enqueue; `ReadBuffer::consumePart` soft-fails bad offsets (see [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md))
- Keep Accept / MediaKey-send / Connect / Poll HTTP **off** Browser IO

## Still open

| Area | State |
|------|-------|
| **m1** desktop matrix | Android ↔ desktop voice without WebRTC |
| Hop peerstore / circuit PeerId dial | media-hop **L1–L3** |
| Teardown libdatachannel | **m2** next |
| Video on libp2p | Deferred |
| Group SoftMigrate in lifecycle | Phase hook reserved; not v1 |
| N≥3 unify engine on libp2p send/recv | N021 follow-on |

## Next agent — start here

1. **m2** or finish m1 desktop dogfood (Android ↔ desktop libp2p voice).  
2. **media-hop L1:** peer address book in vendored libp2p / `PeerSessionManager`.  
3. Mesh [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup); confirm seed `media_relay`.  

## Agent traps

| Wrong | Right |
|-------|-------|
| Reintroduce `call_hop_addrs` / app ICE gather | H007 — reachability **in** libp2p |
| Extend libdatachannel for 1:1 | V026 — libp2p media |
| SoftMigrate invents NAT | Stack dialable? then quote |
| Invent N025 listen from `TopPendingInvite` on tick | Lifecycle `WantEphemeralListen` only |
| Full-shell `SyncLayout` for Accept chrome | `RemountCallChrome` into `#shell-call-*-mount` only |
| Always-mounted `data-if` + Dirty for Accept layer | Presence mount via `RemountCallChrome`; Dirty only for labels/pulse inside a mounted layer |
| Recreate `CallLibp2pMediaBridge` on N025 sync | Only when `CallSessionManager*` changes |
| Call-media `read`/`write` from a non-IO worker while pump runs | `Libp2pHost::Post` async pump only |
| Hold a mutex across blocking stream read from capture | Enqueue + IO-thread write |
| Put Accept / Connect / PollInbox on Browser IO | Dedicated workers / hop off IO |
