# Relay blob upload — phases

**[DESIGN.md](DESIGN.md)** is the authoritative spec.  
**This file orders work only.**

---

## i0 — Project + decisions

- [x] README / DESIGN / DECISIONS / PHASES / CURRENT_STATE
- [x] Index in [projects/README.md](../README.md)
- [ ] Cross-link from [AGENTS.md](../../AGENTS.md) common tasks (optional)

---

## i1 — Shared blob client (foundation)

- [x] `RelayBlobSignPayload` — domains: presign, retain, delete, list, profile-icon (match www bytes)
- [x] `HttpClient::Put(url, body, headers)` with Content-Type / length
- [x] `IBlobClient` + `HttpBlobClient`: presign, retain, delete, list, setProfileIcon, PutUpload
- [x] `UploadRelayBlobBytes` helper (presign → PUT → retain)
- [x] Wire into `ServiceClientFactory` + `ConversationsHub::Blob()` with auth signer
- [x] Unit tests: sign golden vectors; mock upload sequence
- [x] Draft SERVICE_ENDPOINTS section (promote when merged)

**Exit:** Can presign/PUT/retain a test blob from a unit test or dev harness.

---

## i2 — Profile icon upload UX

- [x] Me → Profile: avatar affordance in `settings_section_profile.rml`
- [x] Native image pick (SDL file dialog)
- [x] Client resize/compress to ≤ icon cap (512 KiB default)
- [x] Flow: presign(`purpose: icon`) → PUT → `POST /profile/icon`
- [x] Clear icon path (empty url/blob_id)
- [x] Error handling + progress UI (uploading disables buttons; errors via settings toast)

**Exit:** User can set/clear profile icon; directory shows new `icon` on lookup.

---

## i3 — Icon cache + render

- [x] Parse `icon` on directory hits / user lookup (`DirectoryHit`, `ContactRemote`)
- [x] Local icon cache: `{profile}/cache/icons/…` + meta (`url`, `blob_id`, `fetched_at`)
- [x] Refresh on directory/contacts reload when icon identity changes (url/blob_id compare)
- [x] Render: Me → Profile (i2), contacts list/detail, call chrome immersive roster
- [x] Contact card: render `avatar_url` in `BuildContactCardRml`
- [x] `<img>` from local file path (peer cache + self cache)

**Exit:** Icons visible across app from local cache; manual/directory refresh picks up changes.

---

## a1 — Attachment wire + codec

- [x] `ChatContentType::Attachment` + tail fields (mime, filename, size, url, hash, key material)
- [x] `ChatPayloadCodec` encode/decode + validator
- [x] Content-key AEAD helper for file bytes (separate from envelope AEAD)
- [x] Soft-skip unknown `content_type` on ingest ([R018](DECISIONS.md#r018--soft-skip-unknown-content_type-ships-with-attachments))
- [x] Promote WIRE_SCHEMAS attachment table on merge

**Exit:** Round-trip attachment payload in tests; ingest does not hard-crash on unknown types from peers.

---

## a2 — Composer + upload-before-send

- [x] Attach button in `composer.rml` + native file picker (any type)
- [x] Encrypt file → presign(`octet-stream`) → PUT → retain
- [x] Build attachment `ChatPayload` → normal E2E send path
- [x] Bubble/draft states: uploading → sent / failed ([R015](DECISIONS.md#r015--upload-before-send-for-attachments))
- [x] Group: one blob PUT; pairwise envelopes for payload only ([R005](DECISIONS.md#r005--group-attachments-one-blob-ciphertext-pairwise-key-envelopes))

**Exit:** User can send an attachment in 1:1 thread; message not sent until upload succeeds.

---

## a3 — Receive + display

- [x] Background download queue on attachment ingest ([R008](DECISIONS.md#r008--size-tiered-fetch-on-receive-amended) — a3 queues all sizes via CDN; Smart tier + peer path in a6)
- [x] CDN fetch → decrypt → `{thread_id}/blobs/{hash}` ([R016](DECISIONS.md#r016--content-addressed-local-blob-paths-d075))
- [x] Image inline bubble; video card + OS player open ([R012](DECISIONS.md#r012--local-display-only-os-video-player-for-video))
- [x] Non-media: filename + size; no auto-execute ([R017](DECISIONS.md#r017--dangerous-file-types-never-auto-execute))
- [x] Fallback UI when download fails (retry)

**Exit:** Sent image/video displays from local cache; other files show as saved attachments.

---

## a4 — Quota UX

- [x] Detect presign 429 / quota errors
- [x] Confirm dialog: free relay upload space ([R009](DECISIONS.md#r009--sender-always-retains-quota-pop-is-relay-only-with-confirm))
- [x] `blobs/list` → delete oldest remote via `blobs/delete`
- [x] Copy clarifies: local chat data unchanged

**Exit:** User can recover from quota block without losing local history.

---

## a5 — Hardening (post-MVP slice)

- [x] DEK-wrap local attachment cache (align at-rest policy)
- [x] Video poster frame extraction on receive
- [x] Thread/local storage controls: delete local blobs by thread (UI for [R020](DECISIONS.md#r020--deletion-suppresses-re-fetch))
- [x] Promote SERVICE_ENDPOINTS + freeze ADRs superseded by contracts

---

## a6 — Peer-first blobs + download policy

- [x] Amend receive queue for **Smart** default: auto ≤ 4 MiB; tap > 4 MiB ([R008](DECISIONS.md#r008--size-tiered-fetch-on-receive-amended), [R021](DECISIONS.md#r021--attachment-download-policy-smart-default))
- [x] Pref: Smart / Always auto / On demand; session “Download pending media…”
- [x] libp2p chat-blob protocol: peer-direct transfer by `content_hash` ([R019](DECISIONS.md#r019--peer-first-blob-transfer-cdn-secondary))
- [x] Fetch order: local → peer → CDN; outbound blob: peer when reachable else CDN ([R015](DECISIONS.md#r015--blob-ready-before-send-for-attachments))
- [x] Deletion suppression tombstones; clear-history / delete-file must not re-heal ([R020](DECISIONS.md#r020--deletion-suppresses-re-fetch))
- [x] Wire docs: protocol id + framing note in [LIBP2P_STREAMS.md](../../docs/architecture/LIBP2P_STREAMS.md) / WIRE_SCHEMAS when shipped

**Exit:** Large attachments are tap-to-download by default; missing blobs heal from peer when CDN is gone; cleared history stays cleared on this device.

---

## Agent batch hint

Implement **i1 → i2 → i3** before **a1**. Do not start composer attach until i1 blob client is merged — attachments reuse the same PUT/retain stack. After **a4**, prefer **a6** before deep a5 polish if peer heal / Smart policy are blocking product gaps.