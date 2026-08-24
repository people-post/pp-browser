# Relay blob upload (profile icons + chat attachments)

**Status:** **a1–a3 landed** — **a4** quota UX next; **a6** peer-first + download policy planned  
**Owner:** Hongwei + agents  

**Server contract (www):** [`web2/www/Plans/2026-08-24-relay-blob-upload-design.md`](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md)  
**Stable refs (client):** [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md), [WIRE_SCHEMAS.md](../../docs/contracts/WIRE_SCHEMAS.md), [MESSAGE_ENCRYPTION.md](../../docs/contracts/MESSAGE_ENCRYPTION.md), [DATA_LAYOUT.md](../../docs/contracts/DATA_LAYOUT.md), [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md)  
**Related:** [multi-device-account](../multi-device-account/) (M006 one relay per account), [e2e-message-crypto](../e2e-message-crypto/) (tier keys), [chat-storage-and-memory](../chat-storage-and-memory/) (D075 blobs layout), [at-rest-crypto](../at-rest-crypto/) (DEK-wrapped disk), [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md) (direct-first / history sync)

## One-line goal

Wire pp-browser to Brief **relay blob upload** for **public profile icons** first, then **E2E chat attachments** (any file type; image/video preview in v1). Relay/CDN is a **delivery aid** only — local storage is the source of truth after receive; peer-direct heals missing blobs like history sync.

## Delivery order

| Track | Phases | Summary |
|-------|--------|---------|
| **Icons** | **i1 → i3** | Sign/PUT client, Me → Profile upload, directory + local cache + render |
| **Attachments** | **a1 → a6** | wire, composer, CDN receive, quota, hardening, **peer-first + Smart download** |

Shared foundation: **i1** blob HTTP client (presign / PUT / retain / delete / list / profile icon).

## Locked product shape

| Topic | Decision |
|-------|----------|
| Relay role | Best-effort delivery; **do not rely** on CDN/GC for long-term storage |
| Icons | **Plaintext** public images; client resize ≤ server cap |
| Chat files | **Client encrypt** file bytes; CDN presign always **`application/octet-stream`** |
| Chat wire | One **`attachment`** content type; mime/filename/size/key in E2E `ChatPayload` |
| Envelope send | Direct first, relay fallback (same as text) |
| Blob bytes | **Peer-direct first, CDN second** ([R019](DECISIONS.md#r019--peer-first-blob-transfer-cdn-secondary)) |
| Download policy | **Smart** default: auto ≤ 4 MiB, tap larger; Always / On demand prefs ([R021](DECISIONS.md#r021--attachment-download-policy-smart-default)) |
| After delete / clear history | Suppress re-fetch (no silent peer/CDN heal) ([R020](DECISIONS.md#r020--deletion-suppresses-re-fetch)) |
| Group media | **Encrypt blob once**; pairwise envelopes carry key material only |
| Multi-device private | Same as private text — no PSK → no decrypt on sibling device |
| Quota full | User **confirm → pop oldest on relay only**; **never delete local** |
| UI v1 | Inline **image**; **video** via OS player; other types saved + filename bubble |
| Icon refresh | User-triggered (reload / directory refresh); stamp `url`/`blob_id` for update detection |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Authoritative product + technical spec |
| [DECISIONS.md](DECISIONS.md) | ADRs R001–R021 |
| [PHASES.md](PHASES.md) | i1–i3 + a1–a6 delivery order |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What ships today vs gaps |

## How pieces fit

```mermaid
flowchart TB
  subgraph www [Brief www]
    Presign["POST /v1/blobs/presign"]
    S3["Presigned S3 PUT"]
    Retain["POST /v1/blobs/retain"]
    Icon["POST /v1/profile/icon"]
    CDN["public_url CDN"]
  end
  subgraph client [pp-browser]
    Sign["Relay blob sign helpers"]
    Http["HttpClient PUT"]
    IconUX["Me → Profile icon UX"]
    Cache["Local icon / attachment cache"]
    PeerBlob["libp2p chat-blob a6"]
    E2E["MessageCipher + ChatPayload"]
    Chat["Composer + bubble render"]
  end
  Sign --> Presign
  Presign --> Http
  Http --> S3
  S3 --> Retain
  Retain --> Icon
  Retain --> E2E
  PeerBlob --> Cache
  E2E --> Chat
  CDN --> Cache
  IconUX --> Sign
  Chat --> Sign
  Chat --> PeerBlob
  Cache --> Chat
```
