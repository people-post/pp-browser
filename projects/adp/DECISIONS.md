# Decisions — ADP

## A001 — Name ADP + folder/target names

**Date:** 2026-08-30  
**Decision:** Protocol name **Association Datagram Protocol (ADP)**. Code lives in `src/lib/amp/L1/`, CMake `pp_base_adp` / `pp_browser_adp_test`, project folder `projects/adp/`.  
**Rationale:** Short transport-style name; avoids SCP/UDP collisions; folder matches acronym.  
**Alternatives:** AUP, HDP, “assoc”.

## A002 — L1 HMAC binding vs L2 crypto

**Date:** 2026-08-30  
**Decision:** ADP HMAC authenticates datagram membership (sender binding / anti-inject). Content encryption stays L2. `K_assoc` is supplied out-of-band.  
**Amendment proposed:** [A029](#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H1.3 — a sealed packet's AEAD tag, with the header as its AAD, would do the HMAC's job; the HMAC stays on packets without AEAD (handshake, keepalive). Until A029 is accepted, this entry holds.  
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
**Decision:** Implement in pp-browser `lib/amp/L1` only. No pp-cpp-common / pp-ledger consumer in foundation.  
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

## A022 — Circuit tunnel = non-blocking coordinator on MeshRuntime

**Date:** 2026-08-31  
**Decision:** AMP circuit relay is a **`CircuitTunnelId`-keyed tunnel** on `MeshRuntime`, not a blocking `RequestBridge` + `StreamBridge` port. Public API is non-blocking (`StartBridge` + completion callback). Phase machine and admission live in pure `CircuitBundleLogic`. Runtime (`CircuitTunnelCoordinator`) drives `EnsureAssociation` / `OpenChannel` via **callbacks + `PostToIo` only** — L4 must never call `MeshRuntime::Pump()` / `IoPumpUntil`. `ChannelBridge` remains an io-thread DATA splice helper armed **after** handshake, never via `SetFrameHandler` from inside an active frame handler. D7b media-relay follows this same template.  
**Rationale:** Same failure modes A021 fixed — nested pump reentrancy, handler UAF from mid-callback `SetFrameHandler`, mutex/Stop deadlock, Browser-IO stalls on sync waits. Libp2p circuit blocks on a **worker** lane; AMP put the same waits on the pump thread in transitional D7a.  
**Alternatives:** Keep sync `AmpCircuitRelayService::RequestBridge` with more `PostToIo` workarounds; port `CircuitRelayRuntime::WaitReadyOrStop` onto MeshRuntime.

## A023 — MeshHost may own AmpStack in parallel; same device keys

**Date:** 2026-08-31  
**Decision:** `MeshHost` may construct / attach an `pp::amp::AmpStack` **beside** libp2p without routing product L4 ([A020](DECISIONS.md#a020--single-transport-entry-per-protocol) still forbids dual `if (amp)` in app paths). `enable_amp_stack` **requires** the same `device_ml_dsa_*` keys as `Libp2pHost` so AMP PeerId matches. Amp start failure is **soft** (libp2p stays up; `AmpLastError()`). `MeshHost::Tick` pumps the Amp stack — product must call `mesh->Tick()` (not only `Runtime()->Tick()`). Dial-back / mDNS on ADP are **not** blockers for this ownership step.  
**Rationale:** Cutover needs a live UDP+MSH host object before flipping protocols; separate keys would fork identity; hard-failing Amp would take down production mesh during the parallel phase.  
**Alternatives:** Separate Amp-only process; generate a second PeerId for AMP; hard-fail MeshHost::Start when Amp fails.

## A024 — Amp call-media over circuit = nested Session

**Date:** 2026-08-31  
**Decision:** Amp 1:1 call-media over circuit uses a **nested end-to-end A↔B Session** (inner `Session` + `ChannelMux`) carried on the bridged `ChannelSession`. `CallMediaLegCoordinator` keeps the [A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime) control+media bundle on that virtual link. **Rejected:** single-pipe call-media mode; dual protocol-specific circuit tunnels as the long-term shape. SoftMigrate media-relay may continue to adopt one opaque channel ([D9 step 5c](CURRENT_STATE.md)). See [CALL_MEDIA_CIRCUIT.md](CALL_MEDIA_CIRCUIT.md).  
**Rationale:** Preserves Reliable control vs BestEffort media, authenticates A to B (not relay R), aligns with [A019](DECISIONS.md#a019--circuit-relay--channel-tunnel) blindness, and avoids a second call-media state machine. Opaque single-channel adopt works for media-relay because that L4 is already one pipe — not for the call-media bundle.  
**Alternatives:** Tagged single-pipe hello+media on the splice; two parallel circuit channels (control + media) without inner Session.  
**Amendment proposed:** [A029](#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H3 — the carrier's outer leg authenticates, without encrypting again, the inner packets that are already sealed end to end; the inner handshake stays fully sealed. The nested Session itself is unchanged. Until A029 is accepted, this entry holds.
## A025 — Pre-extract layer cleanup (limits, policies, PeerId)

**Date:** 2026-08-31  
**Decision:** Before extracting `pp-cpp-amp`: (1) channel budgets live in `pp::amp::AmpChannelLimits` under `lib/amp/L3/` (not `base/p2p`); (2) core AMP policy factories stay in `ChannelPolicy.h` (`ControlJson`, `Capability`, `CircuitCarrier`); product L4 factories move to `base/mesh/ProductChannelPolicies.h`; (3) `PeerIdFromMlDsaPublicKey` is its own CMake target `pp_base_peer_id` so mesh/link tests and people do not link `pp_lib_mesh`. Use `AmpChannelLimits` directly (product `MeshChannelLimits` alias removed).  
**Rationale:** Removes the mesh→p2p include edge that blocked shared-lib extract; keeps product protocol sizes out of AMP core; PeerId encoding still needs retained libp2p wire but must not pull MeshHost.  
**Alternatives:** Leave limits in p2p; move PeerId into `pp-cpp-crypto` immediately; keep all factories in `ChannelPolicy.h`.

## A026 — One Session per PeerId under dual-dial (mesh election)

**Date:** 2026-08-31  
**Decision:** `PeerLinkManager` keeps **at most one Connected** non-carrier `PeerLink` (Association + Session) per remote PeerId. Simultaneous A↔B dials elect a winner then drop the loser; **rejected:** keep-both links (dogfood double-free / dangling `ChannelSession` mux pointers).  
**Election:** Prefer the link whose local side is outbound **and** `LocalPeerId > RemotePeerId` (same base58 ordering as [A021](DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime) glare); otherwise keep the already-Connected link. Set `peer_id_to_key_` to the winner; clear protocol handlers on the loser then erase it before (or instead of) applying L4 handlers on a losing inbound. Call-media channel glare (A021) runs on the **elected** mux only.  
**Rationale:** STACK’s one-association-per-peer invariant; dual live muxes made TearDown/CloseQuiet and raw `PeerLink*` handles UB under LAN dual-dial.  
**Alternatives:** Answerer-only UDP (no offerer dial); keep-both forever with shared_ptr link handles (more complex, still doubles NAT/punch cost).

## A027 — Parent-only destroy (L3/L4 ownership hierarchy)

**Date:** 2026-08-31  
**Decision:** Mesh/L3/L4 objects follow a **strict ownership tree**: only the parent may destroy (drop last durable owner / erase map entry / unbind mux handlers) a child. Children may **request** close (`CloseQuiet`, return `false` from `on_frame_`, `on_closed` signal); they must not be the party that drops the durable owner while a mux dispatch is still on the stack. **This specializes the repo-wide rule** in [OWNERSHIP.md](../../docs/architecture/OWNERSHIP.md). Normative mesh detail: [AMP-CHANNEL.md § Ownership hierarchy](../../docs/contracts/AMP-CHANNEL.md#ownership-hierarchy-a027).  
**Tree:** `PeerLinkManager` → `PeerLink`/`ChannelMux` → `ChannelSession` (owned by L4 bundle/session slot) → frame callbacks (non-owning). Mux delivers; it does not own `ChannelSession`.  
**Rules:** (1) one durable owner per `ChannelSession`; (2) no destroy-from-callback that can drop the last owner before dispatch returns; (3) parent unbinds (`ReleaseHandlers` / clear mux handlers) after close — dtor unbind is a safety net only; (4) never touch an object after its container erase; (5) Bind callbacks use `weak_ptr` (+ short-lived pin for the dispatch frame), not cycles into the L4 parent; (6) `PeerLink` drop is manager-only ([A026](DECISIONS.md#a026--one-session-per-peerid-under-dual-dial-mesh-election)); L4 resolves via `peer_key`.  
**Transitional:** TearDown still runs from some L4 frame paths; `ChannelSession::Bind` pins `shared_ptr` for the dispatch so request-close does not UAF. Prefer migrating to “signal → parent drops after dispatch / next io tick.”  
**Rationale:** Dual-dial TearDown-from-callback and media-relay `AdoptClientChannel` post-erase UAF showed child-driven destroy is the dual-dial/fanout crash class.  
**Alternatives:** shared ownership everywhere; forbid any close from `on_frame_` (too rigid for glare/quote one-shot).

## A028 — L4 protocol kinds = seven conversation shapes

**Date:** 2026-09-04  
**Status:** Accepted (design)  
**Normative:** [L4_PROTOCOL_KINDS.md](../../docs/contracts/L4_PROTOCOL_KINDS.md)  
**Amends / cross-links:** [A016](#a016--channel-0--capability--identify-plane), [A019](#a019--circuit-relay--channel-tunnel), [A020](#a020--single-transport-entry-per-protocol), [A024](#a024--amp-call-media-over-circuit--nested-session); mesh [N021](../p2p-mesh/DECISIONS.md#n021--generic-media_relay-framing-qos-channel-types), [N017](../p2p-mesh/DECISIONS.md#n017--split-n4-media-sfu-first-message-relay-separate-pricing-later), [N030](../p2p-mesh/DECISIONS.md#n030--adopt-l4-protocol-kinds-gate).

**Decision:** Freeze L4 taxonomy as **seven conversation kinds** — **identify**, **discover**, **reach**, **circuit**, **rpc**, **blob**, **realtime** — not one `protocol_id` per product feature. AMP L1–L3 already own mux/QoS/crypto; a new L4 id is allowed only when the **conversation contract** is new (QoS/duplex/lifetime, security posture, or failure model). Prefer envelope `op`, `channel_type` / frame fields, circuit `target_protocol`, and PeerLink policy for feature growth. Current shipped ids map onto these kinds; merges/renames are optional and versioned — do not big-bang rewrite wire strings. Carrier (`amp-circuit-carrier`) stays plumbing, not a product kind. Rejected as new kinds: always-on `call-signal`; A/V-specific relay forks; per-feature tunnels; treating deferred `message_relay` as a distinct wire shape (it is an **rpc** host role).

**Rationale:** Stops protocol sprawl while covering find → meet → talk → bulk → live (E2E or blind hop). Aligns existing anti-proliferation ADRs (ch0, generic circuit, generic media hop, V033 RPC vs SM) into one agent-facing gate.

**Converted (2026-09-04):** Product not released — wire ids renamed in-place to kind names (`/pp-browser/rpc|blob|realtime|datagram-relay|circuit|reach/1.0.0`, punch `/pp-browser/reach/punch/1.0.0`). `rpc` kind uses separate OPEN ids `/pp-browser/rpc/chat/1.0.0` and `/pp-browser/rpc/history/1.0.0` (handler ownership; no first-frame demux); punch keeps a distinct reach-kind id (multi-frame SM). Nested-Session carrier uses Amp default `/amp/circuit-carrier/1.0.0` (no product `/pp-browser/circuit-carrier` id).

**Prepared compositions (2026-09-05):** IPFS-*like* content share = **discover** + **blob** (+ **rpc** announce/meta); live broadcast = **rpc** control + **realtime** blind hop (optional **blob** DVR). Documented in [L4_PROTOCOL_KINDS § Prepared compositions](../../docs/contracts/L4_PROTOCOL_KINDS.md#prepared-compositions-no-new-kinds) — no new kinds.

**Alternatives rejected:** One mega “mesh-control” channel for discover+reach+dht (already considered and rejected for DHT in N028); feature-named protocols forever; collapsing E2E call-media and blind media-relay into a single id (different security postures under the **realtime** kind).

## A029 — A smaller packet: one sealed header, derived nonces, no second tag

**Date:** 2026-11-03  
**Status:** Proposed (design, for review). Built so far: step 1, 40 ms Opus packets ([CALLS.md § Audio packets](../../docs/architecture/CALLS.md)); H1–H3 are not. Step 1 measured in the lab (`voice_group_wide`, `voice_pair_wide`, 2026-11-03), per user:
  - group, one stream up: 130 → 88 kbit/s (60 → 35 pkt/s);
  - group, two streams down: 189 → 105 kbit/s;
  - 1:1 through a circuit, each way: up 165 → 112, down 120 → 67 kbit/s. It amends [A002](#a002--l1-hmac-binding-vs-l2-crypto) (H1.3) and [A024](#a024--amp-call-media-over-circuit--nested-session) (H3) once accepted. Wire changes, with no compatibility kept: a major pp-cpp-amp version for H1, pp-cpp-mesh and call media for H2, and a pp-node release.

**Context:** In the lab (`pp_app_lab.py` `voice_{group,pair}_wide`, `video_*`), a 20 ms Opus frame of about 60 B goes out as **265 B** to the hop and **319 B** through a 1:1 circuit. An audio stream at 24 kbit/s costs about 106 kbit/s on the wire to the hop, and that is most of why a call sits at a phone's ~600 kbit/s uplink. A group packet, layer by layer:

| Layer | Bytes |
|---|---|
| Opus | 60 |
| E2E media seal: header (version, seq, mark, channel) + random nonce + tag | 7 + 24 + 16 |
| Relay frame (`MediaDataFrame`: version, stream, channel, type, seq, mark) | 13 |
| L3 frame (version, type, channel id, channel seq) | 10 |
| L2 seal: random nonce + tag | 24 + 16 |
| L2 envelope (kind, channel id, channel seq) | 9 |
| L1 (version, type, association id 16, seq, timestamp, length) + HMAC | 28 + 16 |
| UDP / IPv4 (+ Ethernet in the lab) | 28 (+14) |

A 1:1 circuit adds the outer leg's L3 + L2 seal + envelope + L1 (103 B) and an 8 B length prefix. The duplication: the sequence number 5 times, the channel id twice, a version / type byte at 4–5 layers, two random 24 B nonces, three authenticators (the E2E tag, the L2 tag, the L1 HMAC), a 16 B association id, and length fields that UDP and L3 already give. Video fragments (~1 200 B) pay the same per packet: about 14 %, against 3–4× for audio.

**Decision (proposed):**

*H1: Amp's packet (every Amp packet, chat included; about −59 B):*
1. **One packet header, sealed once.** L1, the L2 envelope and the L3 frame header merge.
   - In the clear: only the type / flags byte, the connection id and the packet number. The receiver needs nothing more before it opens a packet, because the nonce comes from the packet number.
   - Sealed, with the clear header as AAD: the frame type, the channel id and channel seq, and fragment fields on fragments only.
   - A path observer sees less than today, when the L2 envelope shows the channel id and seq. The channel fields belong to the frame, not the packet, so one packet can later carry several frames (a small audio frame with an ack).
2. **The L2 nonce is derived, not sent:** from the direction, the key epoch, the packet space and the association's packet number. Not from the channel seq: channel ids can be reused within a session, and a reused one restarts at 0. L1 counts best-effort and reliable packets separately (`tx_seq_be_`, `tx_seq_rel_`), so the space is in the nonce.
3. **No HMAC on sealed packets.** The AEAD tag over the payload, with the header as AAD, authenticates both; the HMAC stays on packets without AEAD (handshake, keepalive). This amends A002.
4. **An 8 B connection id**, chosen by the receiver at the handshake (was the 16 B association id), and **no L1 length field** (UDP has it).
5. **A 4 B packet number, sent whole.** At its end the association fails as it does today (`SeqWrap`), and the link redials: a new handshake, new keys, numbers from 1.
6. **The datagram, not the payload, has the size limit: 1 232 B of UDP payload.** That fits the smallest MTU IPv6 promises (1 280 B, less 40 for IPv6 and 8 for UDP). The bytes H1 saves go to payload.

*How H1 is built:*
- **L1 seals through an interface and never holds keys.** The nonce needs L1's packet number, so the seal happens where L1 numbers a packet. The session hands L1 a sealer after the handshake, and L1 calls it with the clear header, the packet number and the payload. L1's own tests pass a sealer that does not encrypt, so L1 stays testable without the crypto. QUIC's packet protection is split the same way.
- **The nested session has an inner layout.** A 1:1 call through a circuit runs a Session inside a channel of the outer one ([A024](#a024--amp-call-media-over-circuit--nested-session)), with no L1 of its own.
  - Its packets carry the inner packet number, the sealed frame fields and the tag, and no connection id: the outer channel already says which session they belong to.
  - The inner session keeps its own counter, under the same rules as L1's (item 5, the nonce rules below).
- **The type byte may carry a key-phase bit.** It would tell the receiver which key a packet uses during a rekey's grace window, instead of trying the old key when the new one fails, as `Session::Open` does today. Optional while no product path rekeys.

*What H1 costs:*
- Any change to the packet is a major amp version, shipped together with mesh, pp-node and the apps. One layer can no longer change its wire on its own.
- The sealing, the replay windows, how resends are stored and the handshake framing all move, across amp's three layers. That is why the tests below come before the wire.
- Packet captures can no longer tell channels apart. Debugging relies on amp's logs and the `MetricsLine` counters.
- A junk flood costs the receiver what it does today: one tag check per packet, now the AEAD's instead of the HMAC's.

*H2: one media frame for both call paths, and broadcast (about −30–36 B per media packet):*
7. **One media header:** stream id (u32), channel (u8), flags (mark, keyframe) and seq (u32), once. It replaces the E2E header and `MediaDataFrame`, and drops the 1:1 path's 8 B length prefix (L3 already frames a message).
8. **The E2E nonce is derived** from the media kind, stream id, a sender instance id, the seq and the key epoch. The instance id is 8 random bytes a sender picks at each send-session start and carries in every packet. Net −16 B.

*H3: the carrier's outer leg authenticates, without encrypting, the inner packets already sealed end to end* (1:1 behind NATs, about −24 B after H1). This amends A024.
- The inner handshake, and anything not inner-sealed, stays fully sealed on the outer leg: no one on the path learns who talks to whom.
- What an observer on that leg learns is what the hop already sees: the inner packet numbers. The inner frame fields stay sealed by the inner session.
- The outer leg keeps a tag, so the hop still knows each packet comes from its peer, and nobody can inject into a circuit.

**Rejected: media frames skip the per-link seal.** It would save ~16–24 B more, but the group media header's `stream_id` is a hash of the sender's account (`PublisherStreamIdForIdentity`). Someone on the path between a phone and the hop could then tie its media to an account, and see when that person speaks and has video on. Privacy is not traded for bytes ([projects/privacy](../privacy/)). Revisit only with random per-call stream ids, as a privacy decision first.

**Nonce rules (H1.2, H2.8):** the same key and nonce must never seal two different plaintexts. That breaks XChaCha20-Poly1305 outright (the XOR of the plaintexts, then forgeries).
- **L2:** in each direction and space, the packet number only increases under a key, and never wraps (item 5).
  - L1 resends a reliable packet with its own number and the bytes it stored at the first send. The same ciphertext under the same nonce leaks nothing.
  - The seal therefore moves down to where the number is taken. Today L3 seals before L1 numbers the packet.
  - A rekey (`RequestSessionRekey`, which no product path calls yet) bumps the epoch, and the epoch is in the nonce.
- **E2E:** every member of a call shares its key, both peers of a 1:1 call included, so the stream id and the media kind are in the nonce. The engine counts audio and video separately, both from 1. Today's 1:1 AAD names no sender at all, and the random nonce is what keeps the two directions apart.
  - The instance id covers a sender whose engine restarts under the same key epoch (a path move, a reconnect, back from the background), whose counters restart at 1.
  - It also covers two senders with the same stream id. The stream id is a 32-bit hash of the account, so two devices of one account in a call share it, and the hop does not check that stream ids are unique.
  - Bumping the epoch on every restart instead would mean handing out a new call key each time: rejected.
- **In code:**
  - one function per layer builds the nonce, shared by sender and receiver;
  - the sender refuses to seal when its counter did not move forward under the same key and instance (a bug drops a frame instead of reusing a nonce);
  - the replay windows stay as they are.

**Tests (before any wire ships):**
- uniqueness across audio and video, both packet spaces, the nested session's inner counter, a simulated engine restart under one epoch, two senders with one stream id, rekeys, and reused channel ids;
- fixed known-answer vectors for both layers;
- the lab's per-packet report (`video_*`, `voice_*`) before and after each phase.

**Expected:**

| | Group audio packet | One audio stream on the wire |
|---|---|---|
| Today | 265 B | ~106 kbit/s |
| After H1 + H2 | ~175 B | ~70 kbit/s |
| Plus 40 ms Opus frames (separate, engine-only, +20 ms latency) | ~235 B at half the rate | ~47 kbit/s |

A group video sender's uplink would go from ~520 to ~445 kbit/s.

**Order:**
1. 40 ms Opus frames.
2. H1 (pp-cpp-amp major).
3. H2, with the nonce design reviewed first.
4. H3.

Each phase is measured in the lab and needs a pp-node release before group calls gain.

**Sizes (H1.5, H1.6, H2.8), and why:**
- **Packet number, 4 B.** One association carries well under 2 000 packets/s per space: a member of a ten-person video call receives about 1 350. At that rate 2³² packets lasts over a month, and one redial a month costs nothing.
  - 6 B would only move a limit nobody reaches.
  - A QUIC-style truncated number (2 B on the wire, the rest guessed from the highest seen) saves 2 B. It brings reconstruction edge cases on lossy, reordering paths, under a nonce that depends on the whole number. Not worth it.
- **Instance id, 8 B, not 4.** A clash between two send sessions under one key, with the same stream id, reuses nonces from seq 1. The hop could then read the XOR of two streams and forge on them.
  - With 4 B, the odds are 2⁻³² per pair of sessions. That is small for one call but not over every call ever made, and stream ids already collide (devices of one account).
  - With 8 B, the odds are 2⁻⁶⁴ per pair. The cost is 4 B per media packet.
  - The nonce fits XChaCha's 24 B: kind 1 + stream 4 + instance 8 + seq 4 + epoch 4, with 3 B of zeros.
- **No larger fragments.** Today's datagram is 1 244 B of UDP payload (L1 header 28, payload 1 200, HMAC 16). That is already 12 B over the IPv6 floor, and it works on real paths.
  - After H1, the 1 232 B cap carries about 40–50 B more fragment data per packet than today. A video frame then takes slightly fewer packets, with a datagram no larger.
  - Going past ~1 400 B would need path-MTU probing. A VPN, a WireGuard tunnel (1 420), PPPoE (1 492) or a carrier's tunnel would otherwise drop large packets with no error.
  - Once H1 and H2 cut per-packet overhead, larger fragments would save about 1 % of video bytes. That does not justify the probing or the risk.
