# Relay blob upload — current state

**As of:** 2026-08-24 (**a1–a4** done; **a6** done; **a5** DEK-wrap landed — video poster still open)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R021 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1–i3** | **Done** — blob client + profile icon UX + directory icon render |
| **pp-browser a1–a4** | **Done** — wire/codec, composer send, CDN receive/display, quota UX |
| **pp-browser a6** | **Done** — Smart download, suppression, libp2p chat-blob, fetch ladder, outbound peer upload |
| **pp-browser a5** | **Partial** — clear downloaded attachments UX + DEK-wrap landed; video poster still open |

---

## a6 shipped

| Item | Status |
|------|--------|
| R008 / R021 Smart policy | Auto ≤ 4 MiB; tap larger; pref + backlog drain |
| R020 suppression | Tombstones + clear-history blob wipe |
| R019 peer-first | `/pp-browser/chat-blob/1.0.0` — fetch + push |
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
| Tests | `attachment_download_policy_test`, `chat_blob_responder_test` |

---

## a5 DEK-wrap (landed)

Local attachment cache under `blobs/` is FileCipher-wrapped with magic `PPBA` when the profile DEK is present. Plaintext for UI/OS open is materialized under `blobs_view/` while unlocked and wiped on vault lock. See [AT_REST_ENCRYPTION.md](../../docs/contracts/AT_REST_ENCRYPTION.md).

## Next

| Area | Phase |
|------|-------|
| Video poster on receive | a5 |
| LIBP2P_STREAMS / WIRE_SCHEMAS polish | done (chat-blob in LIBP2P_STREAMS) |
