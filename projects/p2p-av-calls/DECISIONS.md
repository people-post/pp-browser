# P2P A/V calls — decisions

Cross-project refs: [p2p-mesh N009–N015](../p2p-mesh/DECISIONS.md), [push P001–P004](../push-notifications/DECISIONS.md), [group-chat G008](../group-chat/DECISIONS.md), [e2e E022](../e2e-message-crypto/DECISIONS.md).

---

## V001 — Hybrid media stack (Option C)

**Date:** 2026-07-28  
**Decision:** **Signaling and call state** ride the Brief mesh / E2E messaging path. **Media** uses a **WebRTC-shaped** stack (ICE + encrypted realtime media). **Fallback** is mesh Node **`audio_relay` / `video_relay`** SFU (or TURN-like), preferring contact-first then org seed (N014). Do **not** implement a long-lived custom Opus-over-libp2p media stack as the product path.  
**Rationale:** Mobile is always Client (no listen); NAT makes pure full-mesh unreliable; WebRTC delivers expected call quality and congestion control; mesh keeps discovery, SFU ops, and product crypto under Brief.  
**Alternatives:** (A) Full WebRTC including third-party SaaS SFU only — rejected as default (ops may still use extra seeds). (B) Custom A/V solely on libp2p streams — rejected for v1 product quality/time.

---

## V002 — Hostless calls; epoch coordinator = min identity

**Date:** 2026-07-28  
**Decision:** Calls are **hostless**: no end-for-all; session ends when the last participant leaves. For **media key rotation**, the **epoch coordinator** among remaining **joined** participants is the one with the **minimum communicating-identity string** (UTF-8 lexicographic order, e.g. `relay:…`). That peer mints the next `media_epoch` key and fans out `call_media_key`. Coordinator is **not** a product “host” and may change after each leave.  
**Rationale:** Matches hostless UX; deterministic rule needs no election protocol; reuses identity strings already on the wire.  
**Alternatives:** Sticky creator as crypto host (conflicts with hostless leave); SFU mints keys (couples confidentiality to relay); Raft/election (overkill).

---

## V003 — Rotate media key on every leave; overlapping epochs

**Date:** 2026-07-28  
**Decision:** On every participant leave, remaining members **rotate** the shared media key (`media_epoch++`). Implementations **SHOULD** use **overlapping epochs** (accept previous epoch for a short grace, e.g. ~2s) so leave does not hard-cut audio.  
**Rationale:** Prevents ex-participants from decrypting subsequent media; overlap preserves UX.  
**Alternatives:** Rotate only on new call (weaker); rotate without overlap (jarring UX).

---

## V004 — Shared call media key (not group N-ciphertext)

**Date:** 2026-07-28  
**Decision:** All joined call participants share **one** media key per epoch. Key **wrap** to each peer uses existing **pairwise** E2E channels. Do **not** use group-chat N ciphertexts (G008/D095) for RTP/media frames.  
**Rationale:** Realtime fan-out cost; SFU sees one ciphertext class; chat pairwise remains for confidentiality of key distribution.  
**Alternatives:** Per-recipient media encrypt (rejected — efficiency); MLS sender keys for v1 (deferred).

---

## V005 — Call session separate from chat thread; invite-only join

**Date:** 2026-07-28  
**Decision:** Each call has a distinct **`call_id`** and **`call_participants`** roster. Optional link to `origin_thread_id` / `origin_group_id` for history and “start from chat.” **Join requires invite** (including late join and guests). Guests **need not** be added to the chat group. No ambient Join for all group members without invite in v1.  
**Rationale:** Free add/remove of call-only people; simpler authz than open group Join; avoids leaking group history to guests.  
**Alternatives:** Call ≡ group thread (rejected); ambient Join chip (deferred / rejected for v1).

---

## V006 — `call_wake` push shape

**Date:** 2026-07-28  
**Decision:** Introduce opaque push type **`call_wake`**, distinct from **`inbox_wake`**. Payload carries **no** `call_id`, names, or media. Client treats wake as “fetch pending call invites / sync then ring.” Vault-locked behavior follows push P003 (generic incoming-call copy, no decrypt-for-banner).  
**Rationale:** Lets the client prioritize call UI over chat badge; keeps E2E threat model on the push path.  
**Alternatives:** Overload `inbox_wake` only (rejected — weak ring UX); put `call_id` in push (rejected — metadata to FCM/relay).

---

## V007 — Participant cap 16 (soft); engineering floor 8

**Date:** 2026-07-28  
**Decision:** Protocol / product soft maximum **16** joined participants. Until SFU + mobile bandwidth prove out, clients **MAY** enforce **8**. Prefer **SFU topology** for N≥3; do not ship full-mesh at 16.  
**Rationale:** Normal group-call ceiling; avoids early mobile meltdown.  
**Alternatives:** Cap 4 only; full-mesh for small N only as debug.

---

## V008 — Org seed SFU required for mobile v1; more seeds post-release

**Date:** 2026-07-28  
**Decision:** v1 mobile↔mobile assumes at least one **org `pp-node` seed** offering volunteer **audio/video SFU** (mesh capabilities). **Additional SFU seeds** are an explicit **ops** follow-up after release (not blocked on a single seed forever). Desktop friend Nodes with caps on remain preferred via N014 when available.  
**Rationale:** Clients cannot host; without seed SFU, NAT’d mobile calls fail the “normal app” bar.  
**Alternatives:** Desktop-only video until many Seeds (rejected for v1 scope); third-party TURN SaaS as sole path (avoid as default).

---

## V009 — Any group member may start; camera off by default on join

**Date:** 2026-07-28  
**Decision:** Starting a call from a group does **not** require owner role. Video sessions join with **camera off** until the user enables it; mic defaults on (user can mute).  
**Rationale:** Matches common messengers; reduces surprise camera-on.  
**Alternatives:** Owner-only start (rejected); camera-on by default (rejected).

---

## V010 — Parallel delivery: signaling now; media dogfood until seed SFU

**Date:** 2026-07-28  
**Decision:** Ship **a1 (signaling / session / ring)** in parallel with mesh **np→n3→n4 SFU**. **a2+ media** may land on a **LAN / same-network ICE dogfood** path first; **NAT’d mobile green path** waits for org-seed volunteer SFU (V008). Do not block a1 on mesh SFU.  
**Rationale:** Session and ring UX are independently valuable and unblocks UI/crypto wrap work; pretending NAT’d WebRTC works without SFU wastes effort.  
**Alternatives:** Serial “all mesh first” (slows product learning); full media before SFU claiming mobile-ready (false).

---

## V011 — Call state in `profile.db`; media key material vault-backed

**Date:** 2026-07-28  
**Decision:** Persist **`call_sessions`** and **`call_participants`** (and pending invite rows) in **`profile.db`**, not per-thread `thread.db`. Link to origin via `origin_thread_id` / `origin_group_id` columns. **Media epoch key bytes** live in the profile secrets / DEK vault (same class as chat PSKs) — never plaintext in `thread.db`. History hints (`call_started` / `call_ended`) remain normal messages in the origin `thread.db`.  
**Rationale:** Calls span threads and guests; sidebar/active-call queries must not scan every thread DB; aligns with `group_rosters` / `chat_targets` living on profile.  
**Alternatives:** Store sessions only in origin `thread.db` (breaks guests / multi-origin); plaintext keys in SQLite (rejected — at-rest policy).

---

## V012 — Signaling over direct E2E `ChatPayload` system controls

**Date:** 2026-07-28  
**Decision:** v1 call signaling is **`content_type=system`** with call `control_type` values (`call_invite`, `call_accept`, `call_decline`, `call_leave`, `call_roster`, `call_media_key`, `call_ended`, …) carried as **direct** E2E envelopes (pairwise), reusing outbox / ingest / sync. Origin-thread history uses the same payload family locally (and may fan-out as system rows without requiring group N-ciphertext for control). A dedicated libp2p call protocol is **out of v1** unless E2E messaging proves too high-latency for ICE trickle (revisit at a2).  
**Rationale:** Maximum reuse of crypto, delivery, and `call_wake`→fetch path; one less transport to secure.  
**Alternatives:** Always-on libp2p `/pp-browser/call-signal/1.0.0` (deferred); signaling via group N-ciphertext (rejected for guests — V005).

---

## V013 — WebRTC library choice deferred to a2 spike

**Date:** 2026-07-28  
**Decision:** Do **not** vendor a WebRTC stack in a1. Phase **a2** starts with a short **spike ADR** choosing among libwebrtc, a slim datachannel+media helper, or platform Media APIs + custom SRTP — scored on mobile/desktop CMake fit, binary size, SDL/RmlUi video blit, and license. Spike must prove 1:1 Opus on LAN before NAT/SFU work.  
**Rationale:** Wrong library pick is costly; signaling does not need it.  
**Alternatives:** Pick libwebrtc now without spike (rejected).  
**Superseded by:** [V014](#v014--media-stack-libdatachannel--libopus--sdl)

---

## V014 — Media stack: libdatachannel + libopus + SDL

**Date:** 2026-07-28  
**Decision:** Ship a2+ voice media with:

| Layer | Choice |
|-------|--------|
| WebRTC transport | Vendored **libdatachannel** (ICE/DTLS/SRTP/RTP; MPL-2.0) |
| Audio codec | Vendored **libopus** |
| Capture / playback | **SDL3 audio** (enable `SDL_AUDIO`; init in host) |
| Signaling | Existing `ChatPayload` call controls + **`call_sdp` / `call_ice`** |

**Wire crypto (a2):** DTLS-SRTP from libdatachannel provides media confidentiality on the peer connection. The shared call **media epoch key** (V004) is still minted and distributed via pairwise AEAD wrap for rotate-on-leave and for a later app-level frame AEAD / SFU-blind path — not used to replace DTLS-SRTP in a2.

**Rejected for a2:** Full **libwebrtc** (GN build, binary size, BoringSSL conflict); GStreamer `webrtcbin`; LiveKit/mediasoup client SDKs (leaves mesh-owned SFU); custom Opus-over-libp2p (V001).

**Rationale:** CMake-friendly, small footprint, fits vendored `third_party/` + BoringSSL via existing `FindOpenSSL.cmake`, SDL already owns the shell, leaves room for a3 video blit without Chromium.

**Spike exit:** 1:1 Opus on LAN through Brief signaling; document LAN vs NAT in CURRENT_STATE.

---

## V015 — Pairwise wrap AAD for `call_media_key`

**Date:** 2026-07-28  
**Decision:** Inner wrap of epoch key bytes uses `MessageCipher` under the peer's active pairwise session key (`SessionKeyDeriver` + `IPskSessionStore`), channel matching call DMs (`e2e_public` today). AAD string:

`call_media_key|<call_id>|<media_epoch>|<media_key_id>`

Blob encoding matches chat: `EncryptedPayload::EncodeBlob` → base64 as `wrapped_key_b64`. Do not reuse message CanonicalAAD fields.

**Rationale:** Binds wrap to call/epoch; reuses proven AEAD stack without inventing a second cipher.
