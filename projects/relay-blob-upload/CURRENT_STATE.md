# Relay blob upload — current state

**As of:** 2026-08-24 (**a1–a6** + **a5** done — DEK-wrap, video poster, storage UX, SERVICE_ENDPOINTS)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R021 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1–i3** | **Done** — blob client + profile icon UX + directory icon render |
| **pp-browser a1–a4** | **Done** — wire/codec, composer send, CDN receive/display, quota UX |
| **pp-browser a6** | **Done** — Smart download, suppression, libp2p chat-blob, fetch ladder, outbound peer upload |
| **pp-browser a5** | **Done** — DEK-wrap, video poster, clear-attachments UX, SERVICE_ENDPOINTS blob section |

---

## a6 shipped

| Item | Status |
|------|--------|
| R008 / R021 Smart policy | Auto ≤ 4 MiB; tap larger; pref + backlog drain |
| R020 suppression | Tombstones + clear-history blob wipe |
| R019 peer-first | `/pp-browser/blob/1.0.0` — fetch + push |
| Fetch ladder | Pending local → peer-direct → CDN (`AttachmentFetchUtil`) |
| R015 outbound peer | Peer push when reachable; else CDN (`AttachmentClientUtil`) |
| Enqueue peer-only | `CanFetchAttachment` — no CDN URL required when peer/pending path exists |

---

## Key components

| Component | Path |
|-----------|------|
| Smart / Always / On-demand policy | `AttachmentDownloadPolicy.*`, `ProfilePreferences.attachment_download_policy` |
| Background fetch + UI | `AttachmentDownloadService`, `InboxController`, `ChatController` |
| Me → Storage pref + backlog | `StorageSettingsSection`, `settings_section_storage.rml` |
| Suppression tombstones | `AttachmentSuppressionStore.*` |
| Clear-history blob wipe | `PrepareThreadHistoryClear`, `WipeThreadAttachmentBlobs` |
| Peer protocol service | `Libp2pChatBlobService.*`, `ChatBlobResponder.*` |
| Pending push ciphertext | `AttachmentCache` (`blob_cipher/`) |
| Clear all downloaded attachments | Me → Storage → **Clear downloaded attachments…** |
| DEK-wrap local cache | `AttachmentCache` PPBA + `blobs_view/` |
| Video poster | `VideoPosterExtractor.*`, `EnsureAttachmentPoster` |
| Blob HTTP contract | [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md) |
| Tests | `attachment_download_policy_test`, `chat_blob_responder_test`, `attachment_cache_at_rest_test`, `video_poster_extractor_test` |

---

## a5 shipped

| Item | Notes |
|------|-------|
| DEK-wrap | `PPBA` + FileCipher on `blobs/`; view materialization under `blobs_view/` |
| Video poster | Platform extract (AVFoundation / MF / ffmpeg) + soft JPEG; inbox poster `<img>` |
| Storage UX | Clear downloaded attachments (R020) |
| Contracts | Blob routes + sign domains in SERVICE_ENDPOINTS |

## Next

Project MVP complete for planned i*/a* tracks. Optional follow-ups (not blocking): Wi‑Fi-only large auto, in-app preview registry, SERVICE_ENDPOINTS polish as www evolves.
