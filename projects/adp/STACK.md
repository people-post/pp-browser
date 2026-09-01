# AMP stack — big picture

**Status:** Foundation spec (2026-08-30)  
**Owner:** Hongwei + agents  
**Related:** [ADP.md](../../docs/contracts/ADP.md) (L1), [AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md) (L2), [AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md) (L3), [DECISIONS.md](DECISIONS.md) (A001–A026)

## Names

| Name | Scope |
|------|--------|
| **ADP** | L1 wire only — Association Datagram Protocol (`src/lib/amp/L1/`, `pp_base_adp`) |
| **AMP** | Full four-layer peer mesh stack built on ADP — **A**ssociation **M**esh **P**rotocol |

When migrating off TCP/QUIC/Yamux, say **AMP** or **ADP stack**. Reserve **ADP** for the UDP datagram layer.

## North star

Replace the product peer underlay (**TCP + Noise + Yamux**) with **UDP + AMP (L1–L3)**. Application payloads ([WIRE_SCHEMAS.md](../../docs/contracts/WIRE_SCHEMAS.md)) stay at L4 unchanged.

Libp2p shrinks to retained crypto/identity utilities; `Host::newStream` and stream transports retire from the product path ([A017](DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)).

## Four layers

```text
┌─────────────────────────────────────────────────────────┐
│  L4  Application protocols                              │
│      /pp-browser/chat|call-media|media-relay|…          │
│      JSON + binary payloads (WIRE_SCHEMAS)              │
└─────────────────────────┬───────────────────────────────┘
                          │ messages ≤ app max (e.g. 256 KiB)
┌─────────────────────────▼───────────────────────────────┐
│  L3  AMP Channel                                        │
│      mux, OPEN/CLOSE/RESET, fragmentation, QoS policy   │
│      channel 0 = capabilities / identify                │
└─────────────────────────┬───────────────────────────────┘
                          │ AEAD plaintext (L2-full)
┌─────────────────────────▼───────────────────────────────┐
│  L2  AMP Session (FULL)                                 │
│      MSH handshake, PeerId bind, session AEAD, rekey    │
│      derives K_assoc → L1 HMAC                          │
└─────────────────────────┬───────────────────────────────┘
                          │ ADP payloads (≤ 1200 B per datagram)
┌─────────────────────────▼───────────────────────────────┐
│  L1  ADP Wire v1                                        │
│      HMAC bind, BestEffort + Reliable, path migrate     │
└─────────────────────────┬───────────────────────────────┘
                          │ UDP
                    PeerLinkManager
              (dial, warm, backoff, address book)
```

**Horizontal (not a layer):** `PeerLinkManager` — who to dial, warm/cold policy, circuit escalation, preferred multiaddrs. Evolves from today’s `PeerSessionManager`.

## Object hierarchy

Carry forward the three-object discipline from [LIBP2P_STREAMS.md](../../docs/architecture/LIBP2P_STREAMS.md) (pipe vs peer link vs domain session):

| Object | Question | Lifetime | Notes |
|--------|----------|----------|-------|
| **Endpoint** | UDP I/O + L1 demux? | Process | One or more per host; `Endpoint::Pump()` |
| **Association** (L1) | Which `assoc_id` + UDP path? | Until L1 Close / fatal error | Path migrate via `SetPeerEndpoint` |
| **Session** (L2) | Which **PeerId** + crypto epoch? | Until session fail or explicit close | Survives path migrate |
| **Channel** (L3) | Which **protocol / conversation**? | Until RESET or CLOSE | Siblings independent |
| **Domain session** (L4) | What product work? | Call, thread, relay hop | Never on the wire |

### Failure propagation

| Event | Effect |
|-------|--------|
| **Channel RESET** | That channel only; session + siblings continue |
| **Channel CLOSE** | Graceful end; drain or abort per policy |
| **Session rekey** | Channels stay open; new `session_epoch` |
| **Path migrate** (L1) | Session + channels unchanged |
| **Session fail** | All channels on that peer torn down |
| **Association Close** | Session ends |
| **Endpoint stop** | Everything on that socket |

Domain state (call roster, SQLite, jitter buffers) lives on L4 consumers — not on Channel/Session objects.

## One association per peer pair (default)

**One long-lived Association + one Session per remote PeerId**, many Channels on top ([A026](DECISIONS.md#a026--one-session-per-peerid-under-dual-dial-mesh-election)).

| Anti-pattern | Why avoid |
|--------------|-----------|
| New assoc per chat message | Handshake + NAT churn |
| Separate assocs for media vs control | Double punch / double bind |
| Assoc per call | Breaks warm-peer policy |
| Keep both dual-dial links Connected | Two muxes → L4 TearDown / dangling `ChannelSession` |

**Dual-dial:** both sides may dial briefly; mesh **elects** one Connected link (higher base58 PeerId’s outbound wins when both initiated; else keep already-Connected), then drops the loser **on Tick** (never `DropLink` the existing winner’s rival mid-`OnLinkEstablished` — UAF). Call-media OPEN glare stays on that single mux ([A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime)).

**Ownership:** only the parent destroys a child (manager → link/mux → `ChannelSession` → callbacks signal only) — repo-wide [OWNERSHIP.md](../../docs/architecture/OWNERSHIP.md), mesh [A027](DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy), [AMP-CHANNEL § Ownership](../../docs/contracts/AMP-CHANNEL.md#ownership-hierarchy-a027).

Media vs control separation uses **channels + QoS**, not separate UDP associations.

## QoS mapping (fixed at channel OPEN)

| L3 channel class | ADP QoS | Examples |
|------------------|---------|----------|
| Transactional, Control, Bulk | **Reliable** | chat, history, blob, dial-back, call hello/teardown |
| Realtime media | **BestEffort** | Opus, relay media frames |
| Realtime signaling on a call | **Reliable** | same call may use both classes on different channels |

**Rule:** L3 Reliable channels send only on ADP Reliable. Realtime media sends only on ADP BestEffort. Do not stack reliable byte streams on top of unreliable ADP for control.

Port today’s `StreamIoPolicy` concepts (`CallMediaIoPolicy`, `ControlJsonIoPolicy`, …) to **channel policies** at L3.

## L2-full security (summary)

- **MSH** (Message Session Handshake): PQ peer authentication + forward secrecy — message-native transcript; semantic reference [libp2p-pq-transport](../libp2p-pq-transport/DESIGN.md) (ML-KEM-768 + ML-DSA-65).
- **Session profile:** **full only** on the product path ([A013](DECISIONS.md#a013--l2-full-session-only)).
- **Key separation** ([A015](DECISIONS.md#a015--k_assoc-and-k_session-from-msh-transcript)):
  - `K_assoc` → ADP L1 HMAC only
  - `K_session` → L2 AEAD only (never reuse labels)
- **AEAD AAD:** `session_epoch || channel_id || channel_seq || direction`
- L4 E2E (chat body, call-media AEAD) remains; L2 hides metadata (envelope JSON, Identify addrs, relay control).

Transitional call Opus path (`CallMediaAdpPath`, A008–A011) used TCP-hello `K_assoc`; **delete when AMP call-media ships** ([A015](DECISIONS.md#a015--k_assoc-and-k_session-from-msh-transcript)).

## Channel 0 — capability plane

Immediately after Session completes, open **well-known channel 0** ([A016](DECISIONS.md#a016--channel-0--capability--identify-plane)):

- PeerId confirmation, supported L4 protocol ids, listen multiaddrs
- Relay/circuit capability flags
- Replaces libp2p Identify + partial peerstore on the product path

Dynamic protocols use `OPEN` with `protocol_id` (same `/pp-browser/*/1.0.0` strings where applicable).

## Fragmentation

**L1 ADP payload cap stays 1200 B.** Multi-datagram **L3 messages** for L4 frames up to app max ([A018](DECISIONS.md#a018--fragmentation-at-l3-not-l1)) — analogous to today’s `u64` length-prefixed stream frames, but message-native.

## Circuit / media-relay (big-picture)

**Channel tunnel model** ([A019](DECISIONS.md#a019--circuit-relay--channel-tunnel)):

```text
A ──Session──► R ──Session──► B
     tunnel_ch_A              tunnel_ch_B
          └──── R forwards L3 frames opaque ────┘
```

- **v1 (shipped D7a / [A022](DECISIONS.md#a022--circuit-tunnel--non-blocking-coordinator-on-meshruntime)):** `CircuitTunnelCoordinator` splices opaque **L4 DATA** on A↔R and R↔B Sessions via `ChannelBridge` — message-oriented, non-blocking (not a sync `RequestBridge` port)
- **Future:** nested A↔B Session so relay sees only L2 ciphertext (full [A019](DECISIONS.md#a019--circuit-relay--channel-tunnel) blind)
- Target `protocol_id` in bridge JSON (replaces stream-bridge target protocol)

## Addressing

```text
/ip4/<host>/udp/<port>/adp/1.0.0/p2p/<PeerId>
```

Listen: one UDP socket per host (`Endpoint` demuxes many associations). Dial: create association toward `(ip, port)`; MSH binds PeerId.

## Threading — MeshPump

| Thread | Work |
|--------|------|
| **IO** | `Endpoint::Pump` → Session decrypt → Channel demux → L4 dispatch (fast) |
| **Worker** | SQLite history serve, heavy decrypt/parse (unchanged from today) |

L3 channel objects are **io-thread affine** (same rule as `DuplexFrameSession` today). See [THREADING.md](../../docs/architecture/THREADING.md).

**Product pump:** `MeshHost::Tick` → `MeshRuntime::Drive()` is mutex-serialized so Connect waiters (worker `io_pump`) and `MessagingHub::TickLibp2p` (coordinator) may both call Tick without racing PeerLink/Mux.

## Code layout (planned)

```text
src/lib/amp/L1/          → L1 (exists; no libp2p)
src/lib/amp/L2/ → L2 MSH, Session, rekey
src/lib/amp/L3/ → L3 mux, frag, ChannelSession
src/base/p2p/          → PeerLinkManager, reachability; libp2p glue shrinks
```

Acyclic: `crypto` → `adp` → `mesh` → `p2p` → `people`. `mesh` must not link libp2p ([A017](DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)).

## Transition

| Phase | Milestone |
|-------|-----------|
| **A** | AMP stack parallel — `MemoryDatagramIo` + unit tests only |
| **B** | L4 protocols port one-by-one (chat → history → call → relay) |
| **C** | Delete TCP/Yamux/Noise wire path; libp2p Host gone from product |

No scattered `if (amp) … else (libp2p) …` in features — single `ChannelSession` entry per protocol ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol)).

## Non-goals (v1)

- Traffic padding / traffic analysis resistance
- Kademlia DHT as dial requirement (contacts-first mesh)
- QUIC compatibility
- Per-channel lite/full session profiles (session is full only)

## Related docs

| Doc | Layer |
|-----|-------|
| [DESIGN.md](DESIGN.md) | L1 detail |
| [AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md) | L2 normative |
| [AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md) | L3 normative |
| [PHASES.md](PHASES.md) | Implementation checklist |
| [LIBP2P_STREAMS.md](../../docs/architecture/LIBP2P_STREAMS.md) | **Legacy** stream stack (until Phase C) |
