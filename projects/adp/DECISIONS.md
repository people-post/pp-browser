# Decisions — ADP

## A001 — Name ADP + folder/target names

**Date:** 2026-08-30  
**Decision:** Protocol name **Association Datagram Protocol (ADP)**. Code lives in `src/base/adp/`, CMake `pp_base_adp` / `pp_browser_adp_test`, project folder `projects/adp/`.  
**Rationale:** Short transport-style name; avoids SCP/UDP collisions; folder matches acronym.  
**Alternatives:** AUP, HDP, “assoc”.

## A002 — L1 HMAC binding vs L2 crypto

**Date:** 2026-08-30  
**Decision:** ADP HMAC authenticates datagram membership (sender binding / anti-inject). Content encryption stays L2. `K_assoc` is supplied out-of-band.  
**Rationale:** Mobile path churn must not force TLS/QUIC; product already has Noise/AEAD.  
**Alternatives:** QUIC/TLS as transport.

## A003 — Message-oriented connection + path migrate

**Date:** 2026-08-30  
**Decision:** API is connect/send/recv/shutdown over **messages**, not a byte stream. Connection identity is `assoc_id` + key; UDP path may change via `SetPeerEndpoint`.  
**Rationale:** Calls + short RPC fit messages; IP churn is path update, not reconnect.  
**Alternatives:** TCP-like byte stream; reconnect on every IP change.

## A004 — Wire v1 + HMAC-SHA256-128 + LE + max 1200

**Date:** 2026-08-30  
**Decision:** Little-endian fields; version 1; HMAC-SHA256 truncated to 16 bytes over header+payload; max payload 1200; seq `u32` starting at 1.  
**Rationale:** Keeps overhead ~44 B header+tag; fits typical UDP MTU; sodium `crypto_auth_hmacsha256` available.  
**Alternatives:** BE (ledger frames); 8-byte seq; larger max.

## A005 — Reliable = ACK + rtx; BestEffort = no rtx

**Date:** 2026-08-30  
**Decision:** Two QoS classes with **separate seq spaces per direction**. Reliable uses ACK + retransmit on injected clock. BestEffort never retransmits.  
**Rationale:** Matches “improved TCP” mental model without full byte-stream CC.  
**Alternatives:** Hint-only reliable; single shared seq space.

## A006 — Browser-first; extract shared lib later

**Date:** 2026-08-30  
**Decision:** Implement in pp-browser `base/adp` only. No pp-cpp-common / pp-ledger consumer in foundation.  
**Rationale:** Dogfood path migration + lossy/reliable where it hurts.  
**Alternatives:** Start in common or ledger.

## A007 — Asio-free DatagramIo

**Date:** 2026-08-30  
**Decision:** `pp_base_adp` does not link or include Asio. I/O is `DatagramIo` + `MemoryDatagramIo` / `OsUdpDatagramIo`. Timers use injectable `Clock`. Optional Asio adapter is out of scope and never required by the core target.  
**Rationale:** Keep L1 shareable with pp-ledger; deterministic tests without a reactor.  
**Alternatives:** Asio UDP as hard dependency.

## A008 — First consumer = Opus BestEffort

**Date:** 2026-08-30  
**Decision:** First product dogfood of ADP is **1:1 call Opus (channel 0) BestEffort** only. Video, signaling, hello/key exchange, SoftMigrate/`media_relay` stay on TCP+Noise+Yamux.  
**Rationale:** Matches phone IP-churn motivation; ADP v1 max payload 1200 B fits Opus, not typical H264 AUs.  
**Alternatives:** Control RPC first; video-over-ADP with fragmentation.

## A009 — `K_assoc` from call media key

**Date:** 2026-08-30  
**Decision:** Derive 32-byte `K_assoc` via HKDF-SHA256 from the existing call media key. Info string `pp-adp-call-media-v1|call_id:<id>|epoch:<n>`. Never send `K_assoc` on the wire.  
**Rationale:** Reuses call key agreement on TCP hello; ADP stays L1 binding only.  
**Alternatives:** Separate key exchange; send key in hello.

## A010 — Hello extension (TCP) + local IP

**Date:** 2026-08-30  
**Decision:** Additive JSON on call-media `hello` / `hello_ack` (ignore if absent): `adp_v` (1), `adp_port`, `adp_assoc` (32 hex chars), `adp_ip` (dotted IPv4 for outbound dial). Offerer mints `adp_assoc`; answerer echoes it.  
**Rationale:** Need peer IP+port before first Opus packet; TCP hello already runs.  
**Alternatives:** Learn IP only from first packet; use libp2p remote multiaddr only.

## A011 — Fallback to TCP stream Opus

**Date:** 2026-08-30  
**Decision:** If peer omits ADP fields, TEMP dogfood gate `kCallMediaAdpOpusDogfood` is false, bind fails, or path is not alive after grace, Opus uses existing `SendMedia` stream path. Calls must not fail solely because ADP is unavailable. Gate lives in `CallMediaAdpDogfood.h` (not settings) and is deleted when Opus-over-ADP becomes default-on.  
**Rationale:** Safe dogfood; dual-NAT without punch falls back cleanly; no config surface.  
**Alternatives:** Hard-require ADP; persist `libp2p.adp_opus` in config.json (rejected for short dogfood).  
**Status:** **Transitional.** Superseded for product direction by [A012](#a012--amp-native-stack-option-b). Delete with Phase D6.

---

## A012 — AMP-native stack (Option B)

**Date:** 2026-08-30  
**Decision:** Product peer mesh migrates to **AMP** — four layers on ADP L1: Session (L2 full), Channel (L3), app protocols (L4). **Replace** TCP + Noise + Yamux underlay; do not add ADP as a libp2p `TransportAdaptor` shim (Option C).  
**Rationale:** One message-oriented model end-to-end; avoids permanent Yamux-on-ADP impedance mismatch.  
**Alternatives:** Option C (ADP as libp2p transport); stay on TCP indefinitely.  
**Refs:** [STACK.md](STACK.md), [PHASES.md](PHASES.md) D0–D9.

## A013 — L2-full Session only

**Date:** 2026-08-30  
**Decision:** AMP Session profile is **full** on the product path — MSH authenticates PeerId and **AEAD-encrypts all L3 frames**. No lite (auth-only) profile in v1.  
**Rationale:** Parity with Noise today (metadata privacy); single security policy; L4 E2E remains for content.  
**Alternatives:** L2-lite + per-protocol crypto; per-session lite/full negotiation.  
**Refs:** [AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md).

## A014 — One Association per peer pair

**Date:** 2026-08-30  
**Decision:** Default **one long-lived Association + one Session per remote PeerId**; multiplex with L3 channels. Media vs control uses channels + QoS, not separate UDP associations.  
**Rationale:** Minimize handshake and NAT state; matches warm-peer policy.  
**Alternatives:** Assoc per message; separate media/control assocs.

## A015 — `K_assoc` and `K_session` from MSH transcript

**Date:** 2026-08-30  
**Decision:** Derive `K_assoc` (L1 HMAC) and `K_session` (L2 AEAD) from authenticated MSH transcript via separate HKDF labels. **Never** bootstrap `K_assoc` from TCP hello or call media key on the AMP path. Transitional `CallMediaAdpPath` (A009/A010) deletes in D6.  
**Rationale:** Unified security story; L1 binding tied to PeerId proof.  
**Alternatives:** Per-protocol `K_assoc`; retain TCP hello bootstrap.

## A016 — Channel 0 = capability / identify plane

**Date:** 2026-08-30  
**Decision:** Reserve **channel id 0** for capability exchange (PeerId, listen addrs, supported protocols, relay caps) immediately after Session establishes. Replaces libp2p Identify on the AMP path.  
**Rationale:** Single bootstrap surface for reachability without stream protocols.  
**Alternatives:** Embed caps in MSH; keep libp2p Identify on side TCP.

## A017 — libp2p shrink: retain crypto + PeerId only

**Date:** 2026-08-30  
**Decision:** Vendored libp2p **retires** from product path: TCP/QUIC transport, Yamux, Noise wire, `Host`, `newStream`, multistream-select. **Retain** ML-DSA PeerId, multihash, crypto providers (or migrate to `pp-cpp-crypto` later). `base/mesh` must not link libp2p. Amend N022 spirit: mesh semantics continue; libp2p is not the underlay.  
**Rationale:** Clear ownership; AMP owns dial/mux/crypto on UDP.  
**Alternatives:** Keep libp2p Host with ADP transport (Option C).

## A018 — Fragmentation at L3, not L1

**Date:** 2026-08-30  
**Decision:** L1 ADP max payload **1200 B** stays fixed. L3 **Frag** frames reassemble large L4 messages (up to app max).  
**Rationale:** Stable L1; blob/history/chat frames are an L3 concern (like today's `u64` stream frames).  
**Alternatives:** Raise L1 cap; L4 app-level chunking only.

## A019 — Circuit relay = channel tunnel

**Date:** 2026-08-30  
**Decision:** Circuit relay forwards **opaque L3 frames** (L2 ciphertext) between paired tunnel channels on relay↔A and relay↔B sessions. Relay does not terminate Session for blind relay. Tunnel OPEN carries target `protocol_id` + PeerId.  
**Rationale:** Preserves blind relay; raw L1 forward breaks session crypto; session terminate at relay breaks blindness.  
**Alternatives:** L1 datagram splice; full protocol rewrite at relay.

## A020 — Single transport entry per protocol

**Date:** 2026-08-30  
**Decision:** Each L4 protocol uses one **`ChannelSession`** entry point. No scattered `if (amp) … else (libp2p)` in feature code during migration — parallel stacks only in tests until cutover.  
**Rationale:** Prevents dual-stack ossification.  
**Alternatives:** Per-protocol feature flags with dual paths in production.

## A021 — Call-media = channel bundle on MeshRuntime

**Date:** 2026-08-31  
**Decision:** AMP call-media is a **`call_id`-keyed control + media channel bundle** on `MeshRuntime`, not a blocking libp2p-style stream adapter. Bundle phase is AMP-native (`OutboundHello` / `InboundHello` / `AwaitingMedia` / `MediaReady`); glare and channel-close admit rules live in pure `CallMediaBundleLogic`. Channels are role-tagged (outbound control / inbound control / media); provisional closes use `CloseQuiet` / slot-first teardown so abandoned roles cannot fail the winning bundle. L4 is non-blocking (`StartLeg` + completion callback); inbound hello may run on a worker lane with wire ops posted back to the io thread. Dual-dial glare: **higher base58 PeerId** keeps outbound; lower abandons outbound and adopts inbound.  
**Rationale:** Removes IoPump blocking, shared “active control” pointer races, and stream-era phase tables from the AMP path; matches libp2p golden behavior without API parity.  
**Alternatives:** Port `CallMediaDirectService::Connect` blocking semantics to AMP; single combined control+media channel; keep `CallMediaSessionPhase` as the AMP source of truth.
