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
