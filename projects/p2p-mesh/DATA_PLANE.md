# Data plane: crypto, I/O, real-time lanes, shards

**Status:** Proposed (2026-11-10), for review. P1 built (2026-11-10, #574; § P1 results); P2–P4 not started. Accepted parts become N-decisions in [DECISIONS.md](DECISIONS.md); shipped behavior moves to `docs/` ([THREADING.md](../../docs/architecture/THREADING.md), [STRESS_LAB.md](../../docs/ops/STRESS_LAB.md)).

**Scope:** what a node and a client do with every datagram once a link exists: receive, open (the link's AEAD), demux, the L4 work (a hop's fan-out, a bridge's splice, media to the app), seal, send. It excludes dialing, handshakes, control protocols and E2E media keys, and keeps the wire unchanged until a phase says otherwise.

## Where it stands (2026-11-10, measured)

One pump thread per `MeshHost` drives Amp and does all of it, under the runtime's IO lock. The worker pool is idle for the data plane.

| | 120 four-member video calls | 250 relayed pairs |
|---|---|---|
| Crypto (ChaCha20, Poly1305, HChaCha20, memzero, SHA-256) | 39 % | 42 % |
| Kernel: UDP, netfilter, syscalls | 21 % | 16 % |
| libc: memcpy, malloc | 14 % | 15 % |
| Mesh / Amp logic | 24 % | 24 % |

`perf` of the pump, two cores, Release with debug info, pp-node 5.2.0 ([STRESS_LAB.md](../../docs/ops/STRESS_LAB.md)). What it shows:

1. **The crypto is the slow, portable implementation.** pp-cpp-crypto builds libsodium without its SIMD paths: the AVX2 / SSSE3 ChaCha20 and SSE2 Poly1305 objects compile empty (the feature defines and per-file flags are set only for clang-cl). A 1100 B XChaCha20-Poly1305 seal takes 1.43 µs; BoringSSL, already vendored for curl, takes 0.47 µs on the same core. libsodium has no NEON ChaCha20 / Poly1305, so phones run portable C too; BoringSSL has AArch64 assembly.
2. **Every fan-out copy is a seal and a syscall.** A group-call frame reaching three members costs three link seals and three `sendto`; the E2E payload is sealed once by its sender, the link seal is per recipient. Seals are 3× the opens in the calls profile.
3. **Acks cost an HMAC-SHA256 each, sent and checked.** About 8 % of a bridge node, with its memzero: reliable traffic acks each packet, and ack packets are HMAC'd rather than sealed.
4. **Queues are bounded by bytes, not time.** A link's mux holds up to 32 MiB. Under overload, media waits seconds instead of being dropped: at 100 calls, the members already in a call got 60 % of their frames at a p95 of 27 s. *Not reproduced (2026-11-10, Release pp-node 5.3.0, default capacities):* at 100 and 200 calls each second's p95 was 6 ms over ~100k samples a second, with the node refusing attaches past its capacity. Best-effort media never waits in the mux, which queues only reliable frames; it goes straight to the socket and is dropped when the socket is full. P2 waits for a past-capacity run (`--capacity`) before anything is built.
5. **On a host left at the OS socket buffers (208 KiB), the kernel drops first.** At 120 calls the pump is 44 % busy while the kernel drops 2.4k datagrams a second: bursts outrun the buffer between rounds. `99-pp-node-udp.conf` fixes the host; the pump's turnaround still matters.

## Requirements

- **R1 — Adapt to the hardware.** Cores (including a cgroup's CPU quota), crypto instructions (AVX2, AES-NI and NEON present or not), and the socket buffer actually granted are detected at start. The data plane picks its shape from them, and an operator can override each choice (`mesh.data_plane.*`). What was picked is on `/status` and `/metrics`.
- **R2 — Phones pay nothing extra.** Same code, one shard by default. No extra threads or wakeups, and the faster crypto saves battery.
- **R3 — Real time.** Late media is dropped, not delivered. Order is reliable control first, then audio, then video. A queue is bounded by age.
- **R4 — One owner per piece of state** (THREADING.md owner rules, parent-only destroy). A link's keys, sequence numbers, replay windows and mux live on one thread; other threads send it messages.
- **R5 — Measured per step.** Each phase has a stress-lab target (below) and its profile.
- **R6 — No wire change** in P1–P4. Where one would help (acks), it is a separate decision.

## Phases

### P1 — Crypto engine (pp-cpp-crypto)

- **Backend:** the link AEAD (`SessionAead`, XChaCha20-Poly1305) moves to BoringSSL's `EVP_aead_xchacha20_poly1305`. BoringSSL picks its own code per CPU at run time, and its portable C is the fallback. Same bytes on the wire, same API; a test pins its output to libsodium's.
- **libsodium SIMD:** enable its SIMD builds (feature defines and per-file `-mavx2` / `-mssse3`, under its own runtime CPU check). Argon2 and BLAKE2b gain too.
- **Fewer copies:** seal straight into the send buffer, and zero key material only where it is held, not every per-packet scratch buffer.
- **Target:** crypto ≤ 15 % of the pump in the calls profile, the same group calls at about 2/3 of today's CPU. Phones: a measured drop in a call's CPU.

#### P1 results (2026-11-10)

**What shipped:**

- **pp-cpp-crypto v1.2.0–v1.3.1:**
  - Vendors upstream BoringSSL (0.20261105.0, a lean 20 MB tree). pp-browser's `third_party/boringssl` (qdrvm's fork) is gone; curl's TLS uses crypto's targets.
  - Builds libsodium with its SIMD code and 64-bit field arithmetic.
  - Adds `XChaCha20Poly1305::SealInto` / `OpenInto` over BoringSSL.
  - Adds `HmacSha256`, which hashes the key's pads once.
- **pp-cpp-amp v5.3.0 / v5.4.0:** `SessionAead` uses those, and so does `HmacBinder` (finding 3, the acks).
- **No wire change:** the AEAD and HMAC bytes are pinned to libsodium's.

**Measured (x86-64):**

| Step | Before | After |
|---|---|---|
| 1100 B seal: libsodium SIMD fix, then BoringSSL | 1.51 µs | 1.29 µs, then 0.51 µs |
| 40 B ack HMAC | 764 ns | 73 ns |

**Stress lab:**

| Profile | Node CPU before | Node CPU after | Crypto share before | Crypto share after |
|---|---|---|---|---|
| 120 calls | 47% | 34% | 39% | 18% |
| 250 bridges | 28% | 15–20% (4 runs; the load clients bound this host) | 42% | 19% |

**Against the target:**

- **Crypto share of the calls profile:** 18% against ≤15%. What is left is the AEAD itself: the hop seals each frame once per member (finding 2), at 11% for seal, 4% for open and 1.7% for HChaCha20.
- **Node CPU in the calls profile:** 72% of before, against about two thirds.
- **The rest needs a wire change** (a later decision, like the acks): seal once for every member of a hop, or a 12-byte-nonce ChaCha20 without XChaCha's per-packet HChaCha20.
- **Phones:** not yet measured.

### P2 — Real-time lanes (pp-cpp-amp mux, pp-cpp-mesh hop)

- **Stamped queues:** each queued frame carries its enqueue time. A best-effort media frame older than its deadline is dropped at dequeue. Defaults: audio 150 ms, video 300 ms; a keyframe (the `mark` bit the hop already reads) keeps its slot over a delta.
- **Age-bounded classes:** a link's queue has classes (reliable control, audio, video) bounded by age, and the 32 MiB byte cap stays only as a memory backstop.
- **Metrics:** queue age p95, deadline drops per class.
- **Target:** at 2× a node's capacity, the members already in calls see p95 latency within the deadline and a bounded loss, never seconds of delay (today 27 s).

### P3 — Batched I/O

- **Batching:** `recvmmsg` / `sendmmsg` on Linux and Android; a fan-out's copies go out in one syscall. Where the OS lacks them (Apple platforms, Windows), the same interface loops over `sendto`.
- **Buffers:** pooled send buffers, no allocation per copy.
- **Target:** kernel plus libc ≤ 20 % of the pump in the calls profile.

### P4 — Shards (nodes only by default)

- **Shards:** N shards, each a thread that owns a slice of the links end to end: their associations, sessions, mux and send queues. N comes from the cores the process may use (`mesh.data_plane.shards`, default `auto` = min(usable cores − 1, 8) on a node serving others, 1 on a device).
- **Steering:** a datagram reaches the shard that owns its association by the association id in its clear header:
  - **Linux:** an `SO_REUSEPORT` group with an eBPF selector reading that id. No cross-shard hop, and a NAT rebinding (a new 4-tuple) still lands on the owning shard.
  - **Everywhere else, or without eBPF:** one socket, and a thin receive thread that reads the id and hands the datagram to its shard.
- **Cross-shard work is a message:**
  - A hop's fan-out to a member on another shard sends the E2E-sealed frame, sealed once by its sender, to that shard, which link-seals and sends it.
  - A bridge's two legs on two shards splice through a queue each way.
  - Per-shard multi-producer queues, bounded and stamped (P2).
- **Control state stays on the control lane:** a hop's sessions, reservations, admission, link establishment and the L4 protocols stay where they are today (the io lane, now the control lane). The control lane publishes immutable snapshots that the shards read: who is subscribed to what, which link a peer is on. A handshake's result moves the new link to its shard.
- **What adapts:** shards, batch sizes and queue bounds. The capacity defaults (`mesh.capacity.*`) scale with the shards actually running, and the pressure lines read the busiest shard.
- **Target:** group calls and bridges scale near-linearly to N cores; on two cores, at least the single-pump numbers.

### Later, wire changes (each its own decision)

- **Acks:** sealed (Poly1305) rather than HMAC-SHA256, or acknowledging ranges, so a bridge stops paying 8 % for them.
- **Stateless reset:** a restarted node tells a phone its association is gone, with a token from the handshake so it can't be forged. Crash recovery comes down to a clean restart's ~20 s (STRESS_LAB).

## Order and why

**P1 → P2 → P3 → P4.**

- **P1:** the largest share and the smallest change, and it helps phones as much as nodes.
- **P2:** fixes the failure users feel (members far behind under overload), whatever the threading.
- **P3:** takes the next largest share.
- **P4:** the structural change. It comes after the cheap gains, so its own gain is measured on an efficient single shard, and it builds on P2's stamped queues for cross-shard traffic.

## Open questions

- ~~**BoringSSL in pp-cpp-crypto**~~ — decided: pp-cpp-crypto vendors it, and pp-browser's copy is gone (P1 results).
- **eBPF steering on the org seeds:** kernel version and container capabilities (the relays run pp-node in Docker); the receive-thread fallback must be measured on its own.
- **Shard count and the media engine on desktops:** a desktop that hosts calls as a hop (`prefer_local_as_hop`) is a node by role. Does it get shards, and does its media engine's direct send path (THREADING rule 4) keep the IO lock or post to its link's shard?
- **The deadline defaults per class**, and whether a call's adaptation (`path_pressure`) should read the deadline drops.
