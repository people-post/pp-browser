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

## a4 thin in code (still relevant under V026)

| Area | State |
|------|-------|
| Topology | N≥3 → sticky initiator `RankMediaHops` → quote/attach → `call_sfu_attach` |
| Hop pick | Contacts ∪ org seed via `MeshHopPolicy`; PreferInCall; needs dialable **multiaddr** until L1 peerstore |
| Budgets / framing | N019 / N021 on SFU path |
| Legacy 1:1 | PeerConnection + ICE until m2 teardown |

## m1 mobile LAN (2026-08-02)

**Devices:** moto g7 play (`ZY323QRNJ9`) + Samsung SM-T380 (`dc07955772d54e6c`), same Wi‑Fi; package `dev.pp_browser.app`.

**Installed:** debug APK with `CallLifecycle` orchestrator.

**Dogfood matrix:**

- [ ] Accept click logs `phase=…->Accepting event=AcceptClicked` (proves orchestrator got the click)
- [ ] Dialog dismisses without SyncLayout remount; in-call chrome appears
- [ ] Log: `WantEphemeralListen=1` then `Mobile ephemeral listen started … bound=.../tcp/<nonzero>`
- [ ] Log: `MediaDeferred` / `Defer answerer` then `MediaKeyReady` / `BeginSession`
- [ ] Log: `DirectConnected` or `Call-media Connect ok`
- [ ] Decline → `Idle`; Leave → `Idle`; Retry from `ConnectFailed` re-enters connecting
- [ ] Timer advances; mic levels move

Filter: `adb logcat -s pp-browser:W` — `CallLifecycle`, `AcceptIncoming`, `Mobile ephemeral listen`, `Defer answerer`, `Call-media Connect`.

## Still open

| Area | State |
|------|-------|
| **m1** 1:1 voice on libp2p | Orchestrator in tree; **complete dual-device matrix** above |
| Hop peerstore / circuit PeerId dial | media-hop **L1–L3** |
| App AEAD on media frames | Follow-on |
| Teardown libdatachannel | After m1 (**m2**) |
| Video on libp2p | Deferred |
| Group SoftMigrate in lifecycle | Phase hook reserved; not v1 |

## Next agent — start here

1. **Live dogfood:** run the matrix on both Android devices; tick boxes in this file.  
2. **media-hop L1:** peer address book in vendored libp2p / `PeerSessionManager`.  
3. Mesh [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup); confirm seed `media_relay`.  

## Agent traps

| Wrong | Right |
|-------|-------|
| Reintroduce `call_hop_addrs` / app ICE gather | H007 — reachability **in** libp2p |
| Extend libdatachannel for 1:1 | V026 — libp2p media |
| SoftMigrate invents NAT | Stack dialable? then quote |
| Invent N025 listen from `TopPendingInvite` on tick | Lifecycle `WantEphemeralListen` only |
| `SyncLayout` remount for Accept chrome | DirtyWindow / DirtyAll only |
| Recreate `CallLibp2pMediaBridge` on N025 sync | Only when `CallSessionManager*` changes |
