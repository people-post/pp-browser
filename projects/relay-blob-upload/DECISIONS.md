# Relay blob upload — decisions

ADRs for profile icons and chat attachments. Product answers locked **2026-08-24** (planning session; R008 amended + R019–R021 same day).  
Normative summary: [DESIGN.md](DESIGN.md). Server wire: [www relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md).

---

## R001 — Two-track delivery: icons before attachments

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Ship **profile icons** (i1–i3) before **chat attachments** (a1–a6). Shared blob client in i1 serves both tracks.

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

## R008 — Size-tiered fetch on receive (amended)

**Date:** 2026-08-24  
**Status:** Accepted (amended 2026-08-24 — was “eager for all”)  

**Decision:** After decrypt/ingest of an attachment message, apply the active **download policy** ([R021](#r021--attachment-download-policy-smart-default)):

| Default (**Smart**) | Behavior |
|---------------------|----------|
| Plaintext size **≤ 4 MiB** (server small tier) | Auto background fetch → local `{thread_id}/blobs/{hash}` |
| Plaintext size **> 4 MiB** | Placeholder bubble; **tap to download** |

Transport order for any fetch (auto or tap): **peer-direct first, CDN second** ([R019](#r019--peer-first-blob-transfer-cdn-secondary)). Bubbles read **local paths only** once ready ([R012](#r012--local-display-only-os-video-player-for-video)). Suppressed hashes are never auto-fetched ([R020](#r020--deletion-suppresses-re-fetch)).

**Rationale:** Always-eager large blobs punish bandwidth/disk/peer uplink; always-lazy loses small media when CDN GCs. Small-tier auto preserves R002’s “get a local copy soon” for the common case.

**a3 note:** Current code still queues CDN eager for all sizes; Smart + peer-first land in **a6**.

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

## R015 — Blob-ready-before-send for attachments

**Date:** 2026-08-24  
**Status:** Accepted (amended 2026-08-24 — CDN-only → path-ready)  

**Decision:** Do not emit the chat envelope until the **chosen blob delivery path** succeeds:

1. **1:1, peer reachable:** peer-direct blob transfer ACK *or* CDN PUT+retain (fallback) — see [R019](#r019--peer-first-blob-transfer-cdn-secondary).
2. **Peer offline / unreachable / group:** CDN PUT+retain required (group still one shared CDN object per [R005](#r005--group-attachments-one-blob-ciphertext-pairwise-key-envelopes)).

Bubble states: `uploading` → `sent` / `failed`. Failed delivery leaves no “sent” message. Sender always keeps a local plaintext cache copy when send succeeds.

**Rationale:** Same “no orphan sent state” as before; allows direct-first without pretending CDN is the only path.

**a2 note:** Implementation today is CDN PUT+retain only; peer-direct send path is **a6**.

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

## R019 — Peer-first blob transfer; CDN secondary

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** Attachment **file bytes** use the same transport preference as chat envelopes / history sync ([P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md) D058–D060):

| Direction | Order |
|-----------|--------|
| **Outbound blob** | Peer-direct when reachable → else CDN PUT+retain |
| **Inbound missing blob** | Local hash hit → peer-direct by `content_hash` → CDN GET if `url` still valid → failed UI |

The small **attachment envelope** (`ChatPayload`) continues on the normal message send path (already direct-first, relay fallback). A new libp2p protocol (proposed `/pp-browser/chat-blob/1.0.0`) carries ciphertext or plaintext-hash-authenticated bytes — exact framing in a6 design notes; authz = same chat relationship as history sync.

**CDN role:** Offline / unreachable peers, group fan-out durability window, and fallback when peer transfer fails — **not** long-term archive ([R002](#r002--relay-is-delivery-only-local-is-source-of-truth)).

**Rationale:** Matches user mental model (“direct like messages”); peer heal closes the gap after CDN GC that history sync already closes for envelopes.

---

## R020 — Deletion suppresses re-fetch

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** After the user **clears thread history** or **deletes attachment file(s)** on this device:

1. Remove local blob bytes for affected hashes (clear-history: wipe or suppress that thread’s `blobs/`; single delete: remove that hash).
2. Persist a **suppression / tombstone** set (by `content_hash`, scoped per profile or per thread).
3. Auto paths **must not** re-materialize suppressed hashes: CDN queue, peer blob heal, thread-open `EnsureThreadAttachments`, backlog “download once”.
4. Explicit **“Download again”** on a still-visible bubble (if the envelope remains) may clear that hash’s suppression and fetch once — same idea as intentional restore, not silent sync.

Analogous to `history_floor_seq` for messages: intentional local forget stays forgotten on this device.

**Rationale:** Without suppression, peer-first heal and CDN retry would undo Clear history / storage cleanup.

---

## R021 — Attachment download policy (Smart default)

**Date:** 2026-08-24  
**Status:** Accepted  

**Decision:** One profile preference (Me → Chat / Media), plus a session action:

| Pref | Behavior |
|------|----------|
| **Smart** (default) | Auto-fetch ≤ 4 MiB; tap-to-download larger ([R008](#r008--size-tiered-fetch-on-receive-amended)) |
| **Always auto** | Auto-fetch all sizes (subject to [R020](#r020--deletion-suppresses-re-fetch)) |
| **On demand** | Tap-to-download all sizes |

**Session override (not a permanent pref):** “Download pending media…” drains the current backlog under Always-auto rules, then returns to the saved pref.

Threshold **4 MiB** = server small-tier cap (keep UX and quota vocabulary aligned). Later optional: Wi‑Fi-only auto for large (not required for a6).

**Rationale:** Covers size-tier auto, power-user Always/On-demand, and offline catch-up without four overlapping toggles. Large auto over peer uplink remains opt-in (Always / one-shot).

---
