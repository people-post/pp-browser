# Relay blob upload — current state

**As of:** 2026-08-24 (**a3** landed; **R008 amended**, **R019–R021** locked)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R021 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1–i3** | **Done** — blob client + profile icon UX + directory icon render |
| **pp-browser a1–a3** | **Done** — wire/codec, composer send, CDN receive/display |
| **pp-browser a4** | **Next** — quota UX |
| **pp-browser a6** | Planned — peer-first blobs + Smart download policy + deletion suppress |

---

## Decisions locked after a3 (docs only — not coded yet)

| ADR | Summary |
|-----|---------|
| [R008](DECISIONS.md#r008--size-tiered-fetch-on-receive-amended) | Amended: Smart default auto ≤ 4 MiB; tap larger |
| [R015](DECISIONS.md#r015--blob-ready-before-send-for-attachments) | Amended: blob path ready (peer or CDN) before envelope |
| [R019](DECISIONS.md#r019--peer-first-blob-transfer-cdn-secondary) | Peer-direct first; CDN secondary (like history sync) |
| [R020](DECISIONS.md#r020--deletion-suppresses-re-fetch) | Clear history / delete file → no silent re-heal |
| [R021](DECISIONS.md#r021--attachment-download-policy-smart-default) | Pref Smart / Always / On demand + one-shot backlog |

**a3 reality check:** downloads are still **CDN-eager for all sizes**; no peer blob protocol; no suppression tombstones yet.

---

## What landed (a3)

| Component | Path |
|-----------|------|
| Content-addressed cache | `AttachmentCache.*` → `{profile}/threads/{thread_id}/blobs/{hash}` |
| CDN fetch + decrypt | `AttachmentFetchUtil.*` |
| Background download queue | `AttachmentDownloadService.*` |
| Ingest hooks | `P2pMessagingService`, `ChatSyncService` |
| Sender local copy | `MessagingHub::SendAttachmentFromPath` |
| Bubble render + retry/open | `InboxController::BuildAttachmentRml`, `ChatController` |
| OS file open | `PlatformOpenFile.*` |

---

## Next agent — start here

1. **a4** — Quota UX on presign 429 ([PHASES.md](PHASES.md#a4--quota-ux))
2. Then **a6** — peer-first + Smart policy ([PHASES.md](PHASES.md#a6--peer-first-blobs--download-policy)) unless product prioritizes a5 DEK wrap first
3. Manual smoke: send image in 1:1 → peer sees inline preview after CDN download

---

## Still missing

| Area | Phase |
|------|-------|
| Quota UX | a4 |
| DEK-wrap local cache, video poster | a5 |
| Peer blob protocol + Smart download + deletion suppress | a6 |
