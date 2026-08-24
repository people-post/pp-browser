# Relay blob upload — decisions

ADRs for profile icons and chat attachments. Product answers locked **2026-08-24** (planning session).  
Normative summary: [DESIGN.md](DESIGN.md). Server wire: [www relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md).

---

## R001 — Two-track delivery: icons before attachments

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Ship **profile icons** (i1–i3) before **chat attachments** (a1–a5). Shared blob client in i1 serves both tracks.

**Rationale:** Icons exercise sign/PUT/retain/directory/display without new `ChatContentType`, E2E payload design, or composer work.

---

## R002 — Relay is delivery-only; local is source of truth

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** After a successful local copy, clients **must not** depend on relay/CDN for durability. Retained blobs may be GC’d (~90d) on the server. No tracking of upload binding id across re-register.

**Rationale:** Matches messaging model — relay helps deliver, not archive.

---

## R003 — Profile icons are public plaintext

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Profile icons upload **unencrypted** image bytes. Same public posture as directory nickname / relay id. Client resizes/compresses to fit server `iconMaxBytes` (default 512 KiB).

**Rationale:** Icons are directory-visible; encryption adds no user value.

**Note:** Icon presign may use real image `content_type` (jpeg/png/webp) for CDN friendliness; chat blobs use octet-stream (R006).

---

## R004 — Chat attachments: encrypt file once, envelope like text

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** File bytes are **not** inside the 128 KiB `ChatPayload`. Flow:

1. Generate random **content key** (+ nonce).
2. AEAD-encrypt file → PUT ciphertext to S3 as `application/octet-stream`.
3. Put `{ url, mime, filename, byte_length, key material, content_hash, … }` in `ChatPayload` **`attachment`** type.
4. Encrypt that payload with the **normal thread session key** (same tiers as text).

**Rationale:** Preserves E2E threat model and payload size cap; relay stays blind.

---

## R005 — Group attachments: one blob ciphertext, pairwise key envelopes

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Group chat uploads **one** encrypted blob to CDN (not N copies). Pairwise sender-key wire (E022) wraps only the **small** `ChatPayload` containing the content key — same “encrypt once, fan out metadata” idea as call media (V004).

**Rationale:** Avoids N× large ciphertext cost; does not replace E022 for text bodies.

---

## R006 — Presign content_type is always octet-stream for chat

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Chat attachment presign uses `content_type: application/octet-stream` regardless of logical mime. Interpretation lives in decrypted `ChatPayload` only.

**Rationale:** Relay/S3 must not learn file types; future file types need no www allowlist changes.

---

## R007 — Generic attachment content type (not image/video enums)

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** One new `ChatContentType::Attachment` (name TBD in codec) with tail fields for mime, filename, size, url, key material, hash. UI branches on mime: image/video inline in v1; txt/html/pdf/etc. show filename + saved state; preview registry later.

**Rationale:** Extensible without wire bumps per file kind.

---

## R008 — Eager background download on receive

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** After decrypt/ingest of an attachment message, **queue background fetch** from CDN → decrypt to local `{thread_id}/blobs/{hash}` → mark ready. Bubbles read **local paths only**. Tap-to-download is fallback for failure/offline, not the happy path.

**Rationale:** Relay/GC window makes lazy fetch lose history silently.

---

## R009 — Sender always retains; quota pop is relay-only with confirm

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Sender calls `blobs/retain` after successful PUT. On quota failure (429), offer **user-confirmed** “free relay upload space” via `blobs/delete` on **oldest remote** blobs. **Never** delete local copies as part of quota reclaim. No auto relay delete in v1.

**Rationale:** Simple UX; sender cannot know all recipients fetched; local chat history stays intact.

---

## R010 — Multi-device: same rules as private text

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Private-tier attachment decrypt requires the same PSK as text on that device (E025 — not auto-synced). Sibling linked devices may receive envelopes but cannot decrypt without PSK. Public/group tiers follow existing account/conversation key rules.

**Rationale:** Consistent with messaging; no new sync story in this project.

---

## R011 — Icon refresh: user-triggered; stamp for change detection

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** No push/auto-sync for icons. Refresh on app reload, contacts/directory reload, or explicit sync. Local cache stores `url` / `blob_id` + `fetched_at`; re-fetch when directory returns different icon identity.

**Rationale:** Matches manual sync posture elsewhere; avoids background polling.

---

## R012 — Local display only; OS video player for video

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** RmlUi `<img>` uses **local file paths** after download. Video: thumbnail in bubble + open via **OS-provided player** (same platform layer as capture). No inline RML video element in v1.

**Rationale:** Existing texture pipeline is file-based; defers RML video surface.

---

## R013 — Native file pickers (desktop v1)

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Attachment pick uses native OS file dialog (SDL). Mobile constraints deferred until those platforms ship composer attach.

---

## R014 — Render contact_card.avatar_url; treat as snapshot

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Render `avatar_url` on shared contact cards. Field is a **snapshot at share time**; live icon comes from directory `icon` on the contact record.

---

## R015 — Upload-before-send for attachments

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Do not emit the chat envelope until **upload + retain** succeed. Bubble states: `uploading` → `sent` / `failed`. Failed upload leaves no “sent” message.

**Rationale:** Avoids orphan retains and confused delivery state.

---

## R016 — Content-addressed local blob paths (D075)

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Store decrypted attachment bytes under `{thread_id}/blobs/{hash}` where hash is BLAKE2b-256 of **plaintext** (align with D075). Messages reference hash; dedupe re-sends/forwards.

**Rationale:** Space-efficient; enables refcount deletion later.

---

## R017 — Dangerous file types: never auto-execute

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** html/svg/pdf and other non-media types: save locally; show name/size; open via OS only behind explicit user action. No in-app HTML render in v1.

**Rationale:** “Any file type” must not become RCE via preview.

---

## R018 — Soft-skip unknown content_type ships with attachments

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Implement **soft-skip / placeholder bubble** for unknown `ChatContentType` in the same release window as attachment send (per [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md)). Prevents old clients from hard-rejecting attachment messages.

---
