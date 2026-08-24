# Relay blob upload — current state

**As of:** 2026-08-24 (**a1** landed)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R018 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1–i3** | **Done** — blob client + profile icon UX + directory icon render |
| **pp-browser a1** | **Done** — attachment wire + codec + content-key AEAD + soft-skip |
| **pp-browser a2+** | **Next** — composer attach + upload-before-send |

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

1. **a2** — Composer attach button, encrypt + upload-before-send ([PHASES.md](PHASES.md#a2--composer--upload-before-send))
2. Manual smoke: round-trip attachment payload in dev harness (after a2)

---

## Still missing

| Area | Phase |
|------|-------|
| Composer attach + send | a2 |
| Receive/download + bubble preview | a3 |
| Quota UX | a4 |
