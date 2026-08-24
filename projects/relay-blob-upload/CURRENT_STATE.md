# Relay blob upload — current state

**As of:** 2026-08-24 (**a4** done; **a6** partial — Smart policy + suppression landed; peer blob protocol next)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R021 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1–i3** | **Done** — blob client + profile icon UX + directory icon render |
| **pp-browser a1–a4** | **Done** — wire/codec, composer send, CDN receive/display, quota UX |
| **pp-browser a6** | **Partial** — Smart download + suppression; **peer-first libp2p** still open |
| **pp-browser a5** | Planned — DEK-wrap local cache, video poster |

---

## a6 reality check

| R008 / R021 Smart policy | **Landed** — auto ≤ 4 MiB; tap larger; pref + backlog drain |
| R020 suppression | **Landed** — tombstones + clear-history blob wipe |
| R019 peer-first | **Not coded** — CDN-only fetch/upload still |
| R015 outbound peer path | **Not coded** |

---

## What landed (a6 partial)

| Component | Path |
|-----------|------|
| Smart / Always / On-demand policy | `AttachmentDownloadPolicy.*`, `ProfilePreferences.attachment_download_policy` |
| Pending + tap-to-download UI | `AttachmentDownloadService`, `InboxController::BuildAttachmentRml`, `ChatController` |
| Me → Storage pref + backlog | `StorageSettingsSection`, `settings_section_storage.rml` |
| Suppression tombstones | `AttachmentSuppressionStore.*` |
| Clear-history blob wipe | `PrepareThreadHistoryClear`, `WipeThreadAttachmentBlobs` |

---

## Next agent — start here

1. **a6 (remaining)** — libp2p `/pp-browser/chat-blob/1.0.0` + fetch ladder (local → peer → CDN) + outbound peer upload ([PHASES.md](PHASES.md#a6--peer-first-blobs--download-policy))
2. Manual smoke: receive >4 MiB attachment → **Download** placeholder (not auto CDN); clear history → blobs gone + no silent re-fetch

---

## Still missing

| Area | Phase |
|------|-------|
| Peer blob protocol + CDN/peer fetch ladder + outbound peer upload | a6 |
| DEK-wrap local cache, video poster | a5 |
