# Relay blob upload — current state

**As of:** 2026-08-24 (**a2** landed)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R018 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1–i3** | **Done** — blob client + profile icon UX + directory icon render |
| **pp-browser a1** | **Done** — attachment wire + codec + content-key AEAD + soft-skip |
| **pp-browser a2** | **Done** — composer attach + encrypt + upload-before-send |
| **pp-browser a3+** | **Next** — receive/download + bubble preview |

---

## What landed (a2)

| Component | Path |
|-----------|------|
| Any-file native picker | `NativeFileDialog::ShowOpenFileDialog` |
| Encrypt + upload helper | `AttachmentClientUtil.*` |
| Send orchestration | `MessagingHub::SendAttachmentFromPath`, `MessagingFacade` |
| Composer attach UX | `composer.rml`, `ChatController`, upload chip |
| i18n | `chat.attach_file`, `chat.attachment.uploading` |

---

## What landed (a1)

| Component | Path |
|-----------|------|
| `ChatContentType::Attachment` | `ChatPayloadTypes.h` |
| Wire codec (type `5`) | `ChatPayloadCodec.*` |
| Content-key AEAD | `AttachmentContentCipher.*`, `AttachmentContentHash.*` |
| Soft-skip unknown types | `ChatContentType::Unsupported`, validator + ingest |
| Placeholder bubbles | `InboxController::BuildAttachmentRml`, `BuildUnsupportedRml` |
| WIRE_SCHEMAS | `docs/contracts/WIRE_SCHEMAS.md` attachment table |

---

## What landed (i3)

| Component | Path |
|-----------|------|
| `ProfileIconRef` on hits/contacts | `ContactTypes.h`, `ContactJson.cpp`, `ContactsStore.cpp` |
| Per-peer cache keys | `ProfileIconCache.*` |
| CDN fetch | `ProfileIconFetchUtil.*` |
| Contacts / call render | `ContactsController`, `CallController`, RML |

---

## Next agent — start here

1. **a3** — Receive/download, local cache, image/video preview bubbles ([PHASES.md](PHASES.md#a3--receive--display))
2. Manual smoke: send attachment in 1:1 E2E thread; confirm upload chip → sent bubble

---

## Still missing

| Area | Phase |
|------|-------|
| Receive/download + bubble preview | a3 |
| Quota UX | a4 |
| DEK-wrap local cache, video poster | a5 |
