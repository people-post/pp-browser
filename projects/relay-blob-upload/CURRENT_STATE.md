# Relay blob upload — current state

**As of:** 2026-08-24 (**a3** landed)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R018 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1–i3** | **Done** — blob client + profile icon UX + directory icon render |
| **pp-browser a1–a3** | **Done** — wire/codec, composer send, receive/display |
| **pp-browser a4+** | **Next** — quota UX |

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

## What landed (a2)

| Component | Path |
|-----------|------|
| Any-file native picker | `NativeFileDialog::ShowOpenFileDialog` |
| Encrypt + upload helper | `AttachmentClientUtil.*` |
| Send orchestration | `MessagingHub::SendAttachmentFromPath`, `MessagingFacade` |
| Composer attach UX | `composer.rml`, `ChatController`, upload chip |

---

## Next agent — start here

1. **a4** — Quota UX on presign 429 ([PHASES.md](PHASES.md#a4--quota-ux))
2. Manual smoke: send image in 1:1 → peer sees inline preview after download

---

## Still missing

| Area | Phase |
|------|-------|
| Quota UX | a4 |
| DEK-wrap local cache, video poster | a5 |
