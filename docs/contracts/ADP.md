# ADP — Association Datagram Protocol

**Status:** Foundation (v1). Normative wire for `pp_base_adp`.

**Project notes:** [projects/adp/](../../projects/adp/)

## Role

L1 message-oriented UDP **connection** with HMAC sender-binding and path migration. Payload is opaque (L2 crypto elsewhere). Asio-free.

## Wire v1 (little-endian)

| Field | Size |
|-------|------|
| version | 1 (`1`) |
| flags | 1 (packet type) |
| assoc_id | 16 |
| seq | 4 (per QoS × direction; starts at 1) |
| timestamp_ms | 4 (truncated epoch ms) |
| payload_len | 2 (max 1200) |
| payload | N |
| hmac | 16 (HMAC-SHA256 truncated over preceding bytes) |

**Types:** `DataBestEffort=0`, `DataReliable=1`, `Ack=2`, `Close=3`.

## Delivery

- **BestEffort** — no retransmit
- **Reliable** — ACK + retransmit on injectable clock

## I/O

`DatagramIo` + `MemoryDatagramIo` (tests) + `OsUdpDatagramIo` (UDP sockets). No Asio dependency on `pp_base_adp`.
