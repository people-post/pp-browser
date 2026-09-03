# P2P A/V calls — decisions

Cross-project refs: [p2p-mesh N009–N015](../p2p-mesh/DECISIONS.md), [push P001–P004](../push-notifications/DECISIONS.md), [group-chat G008](../group-chat/DECISIONS.md), [e2e E022](../e2e-message-crypto/DECISIONS.md).

---

## V001 — Hybrid media stack (Option C)

**Date:** 2026-07-28  
**Status:** **Superseded by [V026](#v026--libp2p-only-call-media-http--libp2p-networking)**  
**Decision:** **Signaling and call state** ride the Brief mesh / E2E messaging path. **Media** uses a **WebRTC-shaped** stack (ICE + encrypted realtime media). **Fallback** is mesh Node **`audio_relay` / `video_relay`** SFU (or TURN-like), preferring contact-first then org seed (N014). Do **not** implement a long-lived custom Opus-over-mesh media stack as the product path.  
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
**Updated:** 2026-08-18 — **V034** video_lo uses this same rule (one seal per AU; hop copies ciphertext).  
**Decision:** All joined call participants share **one** media key per epoch — **not** one key per subscriber. Key **wrap** to each peer uses existing **pairwise** E2E channels. Do **not** use group-chat N ciphertexts (G008/D095) for media frames. Publisher AEAD-seals each audio or video AU **once**; the hop fans out that ciphertext.  
**Rationale:** Realtime uplink cost — per-target encrypt would multiply video bytes by N−1. SFU stays blind (one ciphertext class). Chat pairwise remains for confidentiality of key distribution.  
**Alternatives:** Per-recipient media encrypt (rejected — N× uplink); MLS sender keys for v1 (deferred).

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
**Status:** **Transport superseded by [V026](#v026--libp2p-only-call-media-http--libp2p-networking)** (libdatachannel PeerConnection no longer product path). Opus + SDL capture/playback remain useful.  
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

---

## V016 — a3 delivery slice: LAN video; mobile wiring included

**Date:** 2026-07-28 (updated 2026-07-31 — LAN dogfood includes macOS + Win↔Mac; a3 closed)  
**Decision:** Phase **a3** ships **desktop + Android + iOS wiring** for 1:1 video on the LAN/same-network ICE path, with H264 locked (V017), camera-off-by-default (V009), shell video surfaces (V018), and unified Opus+H264 / same in-call (V019). Explicitly **out of a3 “done”**:

| Deferred | Where it lands |
|----------|----------------|
| NAT’d mobile ↔ desktop / mobile↔mobile via seed SFU | Mesh **nr → nu → n3 → n4** SFU + call consumer; do **not** claim in a3 |
| STUN/TURN beyond host ICE | With SFU / mesh reachability work |

Same pattern as a2 (V010): LAN dogfood proves media + UI; NAT claims wait for org-seed SFU (V008).

**a3 exit criteria (claimable):**

1. Two devices on LAN: video call → accept → remote video visible when peer enables camera; local preview when self enables  
2. Camera **off** on join until user toggles on; mic defaults on (V009)  
3. Codec preference **H264** in SDP; encode/decode via **platform HW** (V017)  
4. Desktop camera permissions / OS privacy prompts exercised; Android `CAMERA` (+ `RECORD_AUDIO`); **iOS** `NSMicrophoneUsageDescription` + `NSCameraUsageDescription` + `AVAudioSession` play-and-record + `UIBackgroundModes` `audio` (V019) — **wiring complete**; physical iOS device dogfood optional follow-up  
5. Docs: CURRENT_STATE marks LAN video path + mobile wiring; NAT/SFU still unclaimed; Linux video **send** may fail without camera and/or usable HW encoder (accepted); voice continues (V019)

**Dogfood claimed (2026-07-31):** Android ↔ Windows, **Android ↔ macOS**, and **Windows ↔ macOS** bidirectional video; Android→Linux / Windows→Linux / **Mac→Linux** one-way when Linux dogfood host had no camera (receive/display OK). Voice OK on those pairs. **iOS** device dogfood still optional. NAT / seed SFU not claimed. macOS packaged builds need `NSLocalNetworkUsageDescription` for Sequoia LAN ICE.

**Rationale:** Mesh SFU is still pre-nr; blocking a3 on it repeats the false “mobile-ready” trap. iOS A/V session + plist work ships with a3 so mobile shares one codec/UI path; NAT dogfood still mesh-gated.  
**Alternatives:** Full a3 checklist including SFU (rejected — mesh-gated); defer iOS to separate bring-up (superseded 2026-07-30 — wiring-only iOS exit).

---

## V017 — Video codec H264 via platform HW

**Date:** 2026-07-28  
**Decision:** a3+ video uses **H.264 (AVC), Constrained Baseline** in SDP (`Description::Video` + libdatachannel `H264RtpPacketizer` / `H264RtpDepacketizer`). Encode/decode through **OS hardware APIs** behind a thin `IVideoCodec` (YUV/NAL in, NAL/YUV or RGBA out) — not a vendored soft codec as the product path. **VP8 is not the a3 primary.**

**Wire profile (locked):**

| Item | Choice |
|------|--------|
| Codec name in SDP | `H264` |
| Profile | Constrained Baseline (WebRTC-friendly; target HW encoders accordingly) |
| Packetization | libdatachannel H264 RTP helpers |
| Audio | Unchanged Opus (V014) |
| Encode / decode | **Platform HW** (below) |

**Platform backends:**

| OS | API | a3 expectation |
|----|-----|----------------|
| Windows | Media Foundation (prefer); QSV/NVENC later if needed | Primary desktop dogfood target |
| macOS | VideoToolbox | Primary desktop dogfood target |
| Android | MediaCodec | **In a3** dogfood (V019) |
| iOS | VideoToolbox | **In a3** wiring (plist + AVAudioSession); device dogfood optional |
| Linux | VA-API (and/or V4L2 M2M) when present | **Best-effort** — no soft-codec product fallback in a3 |

**Linux constraint (accepted):** Video **send** requires a capture device **and** a usable H264 encoder. Many Linux hosts (VMs, headless, missing iGPU drivers, no camera) fail one or both. a3 dogfood Linux had **no camera** (receive OK from Android/Windows/**macOS**). Do **not** block a3 on universal Linux soft encode. Receiving/decoding may still work when a HW decoder exists.

**Rejected for a3 product path:**

| Option | Why not |
|--------|---------|
| OpenH264 as default | Cisco MPEG-LA coverage requires **their** downloadable binary + install-time download + user toggle + attribution — not a static `third_party/` link like Opus. Building from source drops Cisco’s royalty coverage ([FAQ](https://www.openh264.org/faq.html)). |
| FFmpeg libavcodec as default | LGPL/size; patents unchanged; mobile better served by MediaCodec/VideoToolbox |
| libvpx / VP8 primary | Diverges from H264 lock |
| Full libwebrtc | Rejected (V014) |

**Rationale:** Avoids OpenH264 distribution/patent dance; best battery and quality on Win/macOS/Android; matches long-term mobile path. Linux unevenness accepted for LAN dogfood.  
**Alternatives:** OpenH264 portable soft default (rejected after patent/binary review); FFmpeg (rejected as primary).

---

## V018 — Video capture / render path in SDL + RmlUi shell

**Date:** 2026-07-28  
**Decision:** a3 video uses this pipeline (1:1 LAN):

```text
SDL3 camera (capture) → YUV/RGBA convert → platform HW H264 encode (V017)
  → libdatachannel video Track + H264RtpPacketizer
  → DTLS-SRTP (existing PC)
  → H264RtpDepacketizer → platform HW decode → RGBA
  → persistent GL texture → shell RML video tiles (layout)
```

### Capture

- Init camera on demand: `SDL_InitSubSystem(SDL_INIT_CAMERA)` (same pattern as audio — do not fail window bring-up).  
- Open device only when user **enables camera** (V009); closing camera on disable.  
- Prefer front-facing when `SDL_GetCameraPosition` reports it.  
- **Orientation:** SDL does not apply sensor orientation. Before encode/preview, rotate via `CameraCaptureOrientation` — Android: `ACAMERA_SENSOR_ORIENTATION` + `Display.getRotation()` (CameraX compensation); iOS: conventional facing angles + interface orientation; desktop: identity. Portrait mobile encodes ~360×640 after rotation; desktop ~640×360. **Mobile UI is portrait-locked** (manifest/plist + `SDL_HINT_ORIENTATIONS`) until free rotation + EGL/live re-orient are hardened — display rotation for capture is effectively fixed while locked.  
- **Render fit:** `CallVideoTileRenderer` letterboxes/pillarboxes into `#call-remote-tile` / `#call-local-tile` (do not stretch).  
- Permissions: OS privacy prompts via SDL; Android `CAMERA` (+ runtime) + link `camera2ndk` for metadata; **iOS** `NSMicrophoneUsageDescription` + `NSCameraUsageDescription` + `AVAudioSession` before capture (V016).

### Peer connection

- Always add **audio + video** tracks in the initial offer/answer (V019) — not only when `media_mode == video`, and not via mid-call renegotiation for camera toggle.  
- When camera off: keep the video m-line; do not send frames; remote UI shows placeholder — not a black full-screen surprise.  
- Signal `video_enabled` on participant media (design entity already has `{ audio_muted, video_enabled }`); reuse roster / lightweight control as needed — no new push type.

### Render in shell (chosen approach)

**Chosen: layout-owned tiles + persistent GL texture updates** (not a free-floating post-Present overlay).

| Approach | Verdict |
|----------|---------|
| **A. Shell RML placeholders** (`data-if` video stage) + **persistent `TextureHandle`** updated with `glTexSubImage2D` / `CallbackTextureInterface::SetTextureHandle` | Optional — Rml `CallbackTexture` release calls `ReleaseTexture`/`glDeleteTextures`, so app-owned handles need careful lifetime transfer |
| **B. Custom `<call-video-tile>` element `OnRender` + app-owned persistent GL tex** (`glTexSubImage2D`, letterbox `RenderGeometry`) | **Adopt** — paints in document stacking (below banner/dialogs); app keeps texture ownership; DirtyCallChrome only |
| **C. OpenGL blit after `Context::Render` / `PresentFrame`** | Reject — breaks stacking (video over banner), hit-testing, safe-area |
| **D. Remount full shell when video / Accept appears** | **Forbidden** — destroys chat panes / broke Samsung Accept hit-test. Layer identity uses `RemountCallChrome` (dedicated mounts only); video stage/PiP inside an already-mounted bar stays `data-if` + `DirtyCallChrome` ([WINDOW_SHELL](../../docs/ui/WINDOW_SHELL.md)) |
| **E. `GenerateTexture` every frame** (full reallocate) | Reject for steady state — GC/alloc cost at 15–30 fps |

**UI composition (1:1 a3; V019 — same in-call for Voice/Video start):**

1. In-call chrome is unified once connected: icon mute / camera / leave + meters / elapsed (allowed on voice-started calls too). Compact layout uses a stacked bar so controls stay on-screen.  
2. When remote (or local) video is active, expand an in-shell **stage** (still overlay, not a new nav tab): large **remote** tile; small **local PiP** when local camera on; placeholder / avatar when remote camera off. Compact bar-only when neither side has video frames.  
3. Camera toggle off → on requests permission + opens SDL camera (encode starts then).  
4. Chrome gate: `CallChromeSync` — **layer identity** → `RemountCallChrome`; **labels / pulse / meters / video flags** → `DirtyCallChrome` without remounting the in-call mount (frame pixels update in place).

**Threading:** decode/upload on media thread → hand RGBA or GPU upload to UI thread before `Context::Render` (same discipline as audio level meters today). Never touch GL from the capture thread without a documented share context (prefer UI-thread upload).

**Rationale:** Reuses SDL camera + GL3 render interface already shipping; avoids Chromium; keeps call chrome in the shell model proven in a2.  
**Alternatives:** Full-screen native video widget (rejected for a3 shell unity); GStreamer pipeline (rejected — V014).

---

## V019 — Unified call media shape; Voice/Video entry only

**Date:** 2026-07-29  
**Decision:** Treat **Voice** and **Video** header actions as two familiar entry buttons only. Once the PeerConnection is up, **do not** treat voice-started and video-started calls as different media sessions.

| Rule | Detail |
|------|--------|
| Wire / SDP | **Always** negotiate **Opus audio + H264 video** m-lines in the **initial** offer/answer (every 1:1 call). No mid-call renegotiation for mute or camera. |
| Content policy | Mute / camera change **what is sent** (silence or no frames), not the SDP shape. Open SDL camera + HW encode **only** when Camera is on. |
| Failure model | **Audio is mandatory** for a connected call; **video is best-effort**. Missing HW encoder → Camera fails/disabled, voice continues. Missing decoder / bad bitstream → placeholder tile, voice continues. Do **not** fail `Start()` / accept solely because video HW is absent. |
| Voice + Camera | **Allowed** — Camera toggle on voice-started calls. |
| Remote video | **Show** whenever the peer sends frames (including voice-started calls). |
| `media_mode` | May remain on invite / history (“started as voice/video”) for copy; **runtime media UX is the same**. |
| a3 dogfood | **Win** (Media Foundation) + **macOS** (VideoToolbox) primary; **Android** (MediaCodec + `CAMERA`) + **iOS** (VideoToolbox + plist/session wiring) **in a3**; Linux VA-API best-effort (V017). |
| Encode defaults (a3) | Desktop ~**640×360**; portrait mobile ~**360×640** after orientation @ 15–24 fps; network-adaptive bitrate/resolution **later** if not cheap. |

**Rationale:** One PC setup path; camera/mute never touch offer/answer; users get the UI they expect from two buttons without maintaining two in-call protocols.  
**Alternatives:** Audio-only SDP for voice + renegotiate on camera (rejected — glare, second SDP round, dual code paths); keep voice chrome without Camera (rejected — same in-call model).  
**Supersedes soft language in** V018 peer-connection “renegotiate or include when video mode.”

---

## V020 — a4 requires true SFU; no full-mesh media

**Date:** 2026-07-30  
**Decision:** Phase **a4** (group ≤8, guests, rotate-on-leave) **requires a true selective-forwarding SFU** for media when N≥3. Do **not** ship full-mesh PeerConnections as the group product path (even for LAN dogfood).

| Rule | Detail |
|------|--------|
| Topology | **True SFU** — each participant uplinks once; SFU fans out. **Not** TURN-as-SFU (N−1 PCs through relay). **Not** full-mesh. |
| Audio + video | a4 includes **both** Opus audio and H264 video through the SFU. |
| Codecs | **Reuse** a3 platform HW path (V017–V019). Do **not** expand encode/decode matrix for newer devices/codecs in a4 — that is a separate later slice. |
| SFU hosts | Org **`pp-node`** + desktop **`media_relay`** (blind; volunteer **default on** — N018 / V021). Mobile default Client; call-scoped listen / in-call hop — **V027** / **N025**. |
| Mesh gate | a4 media depends on mesh **n4-media**. Hop pick: **V023** / **N020**. |
| Blindness / migrate | See **V021**. |
| Bandwidth / bills | See **V022**. |
| Pricing | Schema now (rate 0); **regulates** later — not revenue-first (V023 / N020). |
| Out of a4 “done” | Full-mesh group media; peer `message_relay`; paid SFU UI/settle; network-adaptive / new device codecs. |

**a4 exit criteria (claimable when mesh SFU exists):**

1. N=3…8 on SFU: join/leave, multi-invite + mid-call guest, rotate-on-leave (V003), in-call roster (mute/camera; speaking if cheap)  
2. Audio mandatory through SFU; video best-effort (same failure model as V019)  
3. Cap **8** enforced in client until load-tested (V007)  
4. Docs: CURRENT_STATE marks SFU group path; do not over-claim NAT until seed+desktop dogfood covers the intended matrix  

**Rationale:** Full-mesh uplink and glare cost dominate mobile; V007 already forbids shipping full-mesh at 16. Parallel LAN-mesh (a2/a3 style) would create a throwaway topology. True SFU is the durable product path.  
**Alternatives:** LAN full-mesh for a4 dogfood then SFU later (rejected — dual media paths); TURN-only (rejected — still N−1 encode/upload); wait for paid pricing before SFU (rejected — N017).  
**Cross-link:** Mesh [N017](../p2p-mesh/DECISIONS.md#n017--split-n4-media-sfu-first-message-relay-separate-pricing-later).

---

## V021 — Blind media forwarder; 1:1 P2P; soft migrate to group SFU

**Date:** 2026-07-30  
**Decision:** Refine V020’s “true SFU” into a **homegrown blind selective forwarder** (mesh **n4-media** / N018). The relay **must not** learn media contents: no call media keys on the relay, no codec decode, no audio-vs-video inspection of payloads.

| Topic | Rule |
|-------|------|
| **Blindness** | Relay forwards opaque media datagrams / framed blobs. It may use **routing metadata only** (publisher/subscriber ids, stream ids, byte counters, session membership). Payload confidentiality = **app-layer AEAD under the shared call media key** (V004) end-to-end among participants. DTLS/SRTP to the hop (if any) does **not** replace E2E call-key protection on the SFU path. |
| **One module** | Single **`media_relay`** service (not separate audio/video decode pipelines). Capacity is expressed as **bandwidth / byte budget**, not “this is video.” |
| **Client camera policy** | Driven by **A↑** / session **B↑** (V022): if allowance cannot support video uplink class, **disable Camera**. Relay enforces by **dropping / rate-limiting by size**, never by inspecting content. |
| **1:1** | Stay **direct P2P** when N=2 and ICE works (a2/a3 path). |
| **Invite → N≥3** | **Same `call_id` / session** — do **not** end-and-restart a new call. Soft-migrate: coordinator picks SFU → all joined peers attach → tear down the 1:1 PC. UX may briefly show reconnecting; product copy must not feel like “call ended.” |
| **N drops to 2** | **Stay on SFU** until hangup for v1 (avoid P2P↔SFU flip-flop). Optional later: re-P2P when alone-as-pair. |
| **Who picks SFU** | **Initiator** at start; thereafter **epoch coordinator** (V002) applies the **same pick policy** on invite-to-group, hop failure, or other reestablish events → **re-pick** and reattach. |
| **Stack** | **Own** forwarder on `pp-node` / desktop Node runtime — relay only; no vendored LiveKit/mediasoup as the product path. |
| **Hosts** | Org seed + desktop Node; capability **default volunteer on** when Node is on (N018). Mobile: default Client; **V027** call-scoped listen on Wi‑Fi. |

**Rationale:** Aligns confidentiality with V004 (SFU must not need keys); bandwidth-limited friend Nodes are real; soft-migrate preserves history/roster/key epoch continuity better than tear-down; staying on SFU after N→2 keeps one media code path.  
**Alternatives:** Classic DTLS-terminating media SFU that sees clear RTP (rejected — contents exposure); hard end/restart call on 3rd invite (rejected — worse UX); separate audio/video relay processes that classify codecs (rejected — needs content awareness); return to P2P when N=2 in v1 (deferred — flip-flop risk).  
**Cross-link:** Mesh [N018](../p2p-mesh/DECISIONS.md#n018--blind-media_relay-bandwidth-budgets-volunteer-default-on).

---

## V022 — Media relay bandwidth (↑/↓) + quote; no surprise payer bills

**Date:** 2026-07-30  
**Decision:** Define **upload and download** budgets separately for blind `media_relay` sessions, with a **pre-attach quote** so the **session payer** never gets a surprising bill. SFU **pick priority ranking remains TBD** (separate discussion); this ADR only locks metering / caps / billing UX shape. Mesh twin: [N019](../p2p-mesh/DECISIONS.md#n019--media_relay-updown-budgets-quotes-no-surprise-bills).

### Perspectives

| Perspective | Role |
|-------------|------|
| **Participant** | Bound by per-user uplink / downlink allowances |
| **Payer** | Settles metered usage (v1: **call initiator**, sticky for the session) |
| **Relay** | Advertises capacity, grants session budgets, enforces byte caps, may charge later (N010) |

### Budget numbers (bytes/s or equivalent)

| Symbol | Name | Meaning |
|--------|------|---------|
| **A↑** | `per_user_uplink` | Max this participant may **send** to the relay |
| **A↓** | `per_user_downlink` | Max this participant may **receive** from the relay |
| **B↑** | `session_uplink` | Max aggregate **ingress** for this call on the relay |
| **B↓** | `session_downlink` | Max aggregate **egress** for this call on the relay |
| **C↑ / C↓** | `node_capacity_up` / `node_capacity_down` | Relay global ceilings; session budgets ≤ remaining capacity |

Camera / send policy uses **A↑** (and session **B↑**). Receive / subscribe pressure uses **A↓** and **B↓**. Relay still never classifies audio vs video — only byte volume (V021).

### Quote + no surprise bills

1. **Before attach** (and before soft-migrate 1:1→SFU): coordinator requests a **quote** from the candidate hop for estimated N + video intent.  
2. Quote includes: proposed **A↑/A↓**, **B↑/B↓**, pricing mode/rate (0 if volunteer), **usage estimate**, and a hard **billing ceiling** (max chargeable bytes and/or max settlement amount for this accept).  
3. **Payer explicitly accepts** the quote (volunteer: accept is still required for caps; money = 0).  
4. Relay **must not bill above** the accepted ceiling. Enforcement = rate-limit / drop / force Camera off when caps hit — not open-ended metering.  
5. **Re-quote + re-accept** when N grows, Camera would exceed **A↑**, re-pick hop, or payer would exceed ceiling. No silent upsell.  
6. Paid settle UI may ship later (N017); **schema and quote flow are designed now** so volunteer=`rate 0` uses the same path.

**Rationale:** Fan-out makes download dominate relay cost; splitting ↑/↓ matches physics. Initiator-as-payer is the simplest hostless rule. Ceilings prevent bill shock when pricing turns on.  
**Alternatives:** Single combined bps (rejected — hides fan-out); bill without quote (rejected); each participant pays own share in v1 (deferred); coordinator as payer (rejected — initiator is clearer for “who started the call”).  
**Pick policy:** Locked in **V023** / mesh **N020** (was TBD when V022 shipped).

---

## V023 — Media hop pick: short-term closed set; pricing regulates later

**Date:** 2026-07-30  
**Decision:** Hop selection for blind `media_relay` is a **risk-aware scorer over eligibility classes**, not a hardcoded contacts→seed→public list. **Making money is not the product goal**; the **pricing model exists to regulate** scarcity, strangers, and abuse over time. Mesh twin: [N020](../p2p-mesh/DECISIONS.md#n020--media-hop-pick-short-term-closed-set-pricing-as-regulation). **Scope escalation (outer loop):** [N023](../p2p-mesh/DECISIONS.md#n023--relay-scope-and-domain-bridging-not-geography-tiers) / [RELAY_SCOPE.md](../p2p-mesh/RELAY_SCOPE.md) — short term still closed set; `link`/`site` boost same-LAN contacts only.

### Thesis

> Volunteer + **closed eligibility** first; quote/pricing **schema always present** (rate 0); paid / public classes unlock later to **ration capacity and gate untrusted hops** — revenue is not the success metric. Abuse / flood / fraud outrank friend preference and cheapness.

### Short term (must-have for a4 / n4-media)

| Rule | Detail |
|------|--------|
| **Feasible set** | **Contacts ∪ household/trusted ∪ org seed** only. **No open public** media_relay market. |
| **Auth** | Attach only with authenticated call session (call_id + roster proof from coordinator). |
| **Capacity** | V022 ↑/↓ fit required; byte enforce on relay. |
| **Quote** | V022 quote + ceiling; initiator accept; volunteer rate = 0 on same path. |
| **Pick** | Filter (eligible, auth, ↑/↓ fit, not excluded) → score (**affinity + quality floor + capacity residual**; price = 0) → quote/accept. |
| **Friends vs quality** | Affinity is a **bonus**, not a veto: contact below quality floor is skipped. |
| **Re-pick** | On hop failure / cool-down exclude; same policy. |
| **Provider** | Prefer serving contacts; limit or refuse strangers on volunteer desktop Nodes. |

Expected UX often looks like “friend then seed” — that is an **outcome** of the closed set + score, not a stage machine to hardcode.

### Mid term (pricing as regulation)

| Step | Regulatory effect |
|------|-------------------|
| Curated public class (directory / allowlist) | Controlled entry |
| Paid rate on public / overflow capacity | Ration when free C is scarce |
| Friends: free for contacts; paid or refuse strangers | Preference without special-case tiers |
| Stronger quality + failure history in score | Punish bait hops |
| Soft concentration penalty | Slow single-operator capture |
| Re-quote when leaving volunteer ceiling | Money only when user opts into more capacity |

### Long term (ecosystem)

Bonds/stake for public relays; receipts / soft reputation; anti-dumping (outlier cheap = higher risk); stronger anti-concentration + optional multi-homing; optional paid seed overflow for ops sustainability — **not** “we sell SFU” as mission.

### Explicit non-goals (short term)

Open public directory; paid settle UI; pure `min(price)` sort; hardcoded N014 stage list as the algorithm.

**Rationale:** Closed set removes sybil/cheap-bait/capture for v1; scorer + quote schema avoid a rewrite when regulation via price turns on; friend preference without guaranteeing flaky home uplinks.  
**Alternatives:** Hardcoded priority list (rejected — brittle, gameable); open public + min price in v1 (rejected — abuse); revenue-first paid SFU (rejected).  
**Updates:** Softens “pick TBD” in V020–V022; aligns with N014 as **illustrative outcome** (see N020).

---

## V024 — Adaptive call media (1:1 P2P and SFU); generic relay channels

**Date:** 2026-07-30  
**Updated:** 2026-07-30 — policy applies to **1:1 P2P** as well as group/SFU  
**Decision:** One **adaptation policy** for all calls: priority **audio ≫ video_lo ≫ video_hi**, producer rate control first, prefer fluent low video over stalled high. **Transport backends differ**; do not force 1:1 through `media_relay` when P2P ICE works. Mesh hop framing: [N021](../p2p-mesh/DECISIONS.md#n021--generic-media_relay-framing-qos-channel-types). **No new codec matrix** in a4 — reuse Opus + H264 HW (V017–V019).

### Same policy brain, two backends

| | **1:1 P2P** (ICE OK) | **SFU / group** (N≥3 only; see V025) |
|--|----------------------|--------------------------------------------------|
| Transport | Existing libdatachannel PeerConnection / RTP | `media_relay` + N021 framing |
| Fan-out | N/A (one peer) | Subscribe `(stream_id, channel_id)` |
| Producer | Same: publish audio; lo/hi by uplink + network | Same |
| Receiver demand | Peer feedback / “want hi” via signaling or datachannel | Subscribe set to hop |
| Stale video / audio FIFO | Sender rate + receiver jitter/playout; stack may drop late video | Relay `latest_lossy` / `reliable_ordered` (N021) |
| ↑/↓ quotes (V022) | N/A on pure P2P; apply when a hop is used | Full A/B/C + quote |

**Agents:** Implement adaptation as a **shared module** (publish set, bitrate ladder, focus-hi). Wire P2P and relay as backends. Soft-migrate 1:1→group (V021) keeps the same roles so policy does not restart from scratch.

### Priority

```text
audio  ≫  video_lo  ≫  video_hi
```

Audio is mandatory for a connected call; video is best-effort (V019). Prefer fluent low video over stalled high video.

### Call track mapping (app layer)

| App role | Publish when | Semantic QoS | On `media_relay` `channel_type` |
|----------|--------------|--------------|--------------------------------|
| **audio** | In call (unless muted) | FIFO; even if late | **`reliable_ordered`** |
| **video_lo** | Camera on + uplink/network allow | Prefer latest under loss | **`latest_lossy`** (`mark` on IDR) |
| **video_hi** | Spare uplink + stable; encode only if useful | Prefer latest under loss | **`latest_lossy`** |

On P2P, map the same roles to RTP/tracks; QoS semantics are identical even if headers differ.

### Three decision layers (cost order)

| Layer | Who | Decision |
|-------|-----|----------|
| **1. Producer** (cheapest) | Sender | Send rate + layers (reserve audio; then lo; then hi). Stop hi→lo under congestion. |
| **2. Receiver** | Each participant | Demand: need lo vs hi (and whose video in group). P2P = feedback; SFU = subscribe. |
| **3. Path** | P2P stack and/or relay | Last resort: drop stale video; never skip-to-latest on audio. Relay uses N021; P2P uses local/playout policy. |

### Relay-only details (when hop is used)

- Fan-out by subscription; byte **B↓** pressure → per-`channel_type` policy (N021).  
- Shed order: **video_hi → video_lo → never sacrifice audio FIFO**.  
- Relay is **not** the primary rate controller.

### Feedback

Demand signals (“want hi?”, subscribe set) inform producers so they do not encode/upload unused layers. Heavy video loss may trigger consumer→producer keyframe/`mark` request via call signaling (E2E).

### Phasing

| Slice | Scope |
|-------|--------|
| **a4 must (SFU)** | N021 framing + subscribe; audio + ≥1 video `latest_lossy`; producer rate + Camera; relay stale-drop |
| **a4 / follow-on (1:1)** | Same priority ladder + producer rate on P2P path (may start as single video layer); share policy module with SFU |
| **a4 polish / a5** | Full **video_lo + video_hi**; focus-only hi; encode-hi-only-when-useful on both backends |

**Rationale:** Users need fluent A/V on volatile links in 1:1 too; duplicating policy only for group causes drift. Shared brain + different pipes matches V021 soft-migrate.  
**Alternatives:** Adaptation only on SFU (rejected — 1:1 regresses); force all 1:1 via relay (rejected — extra hop when P2P works); always encode hi+lo (rejected on weak mobiles).  
**Updates:** SFU column “1:1 ICE fail → hop” superseded by [V025](#v025--no-auto-sfu-for-11-ice-fail-retry-on-p2p).

---

## V025 — No auto-SFU for 1:1 ICE fail; Retry on P2P

**Date:** 2026-07-31  
**Decision:** Auto `media_relay` attach is **group-only (N≥3)**. Plain 1:1 must not enter SFU attach-wait or “group needs media_relay” UX when ICE fails or the PC closes.

| Path | Rule |
|------|------|
| **N=2** | Stay on P2P. On ICE `failed` or connect timeout (~15s): mark connect-failed; keep session; UI shows honest failure + **Retry** (rebuild PC as offerer). Do **not** auto-leave and do **not** start SFU. |
| **N≥3** | Soft-migrate (V021) + ICE-fail → SFU recovery remain wired. |
| **Future NAT 1:1** | May use a hop only via an **explicit** product path / ADR — not by binding ICE `failed` to `ShouldUseMediaRelay`. |

**Rationale:** Auto 1:1→SFU misfired attach-wait and group-only toasts when no hop existed; LAN/Local Network failures need Retry + OS tips, not a group path.  
**Alternatives:** Auto SFU on 1:1 ICE fail when hop exists (deferred — false group UX); leave call on timeout (rejected — Retry is better).  
**Cross-link:** [CALLS.md](../../docs/architecture/CALLS.md) topology rules; `CallMediaTopology::ShouldUseMediaRelay` is N≥3 only.  
**Status:** Partially superseded by [V026](#v026--libp2p-only-call-media-http--libp2p-networking) — ICE Retry is legacy; 1:1 undialable peers use libp2p hop / circuit (mesh), not PeerConnection rebuild.

---

## V026 — Libp2p-only call media; HTTP + libp2p networking

**Date:** 2026-07-31  
**Decision:** Product networking is **HTTP + libp2p only** ([NETWORKING.md](../../docs/architecture/NETWORKING.md)). Call **media** (1:1 and group) uses **libp2p** — direct streams and/or blind `media_relay` — with **app-layer E2E** under the shared call media key (V004). **Voice-first**; video on libp2p is deferred. Do **not** extend WebRTC/libdatachannel/ICE as the product path.

| Topic | Rule |
|-------|------|
| **N=2** | Prefer **direct libp2p** media between dialable peers; if undialable, use mesh hop / circuit (explicit path — not ICE Retry). |
| **N≥3** | Blind `media_relay` star (V020/V021 topology intent retained; transport is libp2p only). |
| **Signaling** | Keep system `call_*` controls; `call_sdp` / `call_ice` are **legacy** (remove with teardown). Hop reachability is **in-libp2p** ([media-hop-reachability](../media-hop-reachability/) H007 — no app addr gather). |
| **HTTP** | Preferred for org backend (Brief, billing UX) when reachable. |
| **Settle** | HTTP backend preferred for price/settle; **chain settle backup** when HTTP unavailable ([N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup)). |
| **Teardown** | Remove libdatachannel PeerConnection path from product on a dedicated phase after voice-on-libp2p dogfood; until then treat PC code as legacy. |

**Supersedes / updates:** [V001](#v001--hybrid-media-stack-option-c) (hybrid WebRTC); product intent of [V014](#v014--media-stack-libdatachannel--libopus--sdl) (libdatachannel transport); [V021](#v021--blind-media-forwarder-11-p2p-soft-migrate-to-group-sfu) “1:1 stay on ICE P2P”; [V024](#v024--adaptive-call-media-11-p2p-and-sfu-generic-relay-channels) dual WebRTC/SFU backends → one mesh media backend family; [V025](#v025--no-auto-sfu-for-11-ice-fail-retry-on-p2p) ICE Retry as product recovery.

**Rationale:** One peer stack to deepen (NAT, discovery, QoS, incentives); HTTP for backends; accept realtime/NAT tradeoffs for **audio-first**; avoid maintaining ICE and multiaddr dial in parallel.

**Alternatives:** Keep WebRTC 1:1 + libp2p SFU (prior V001 — rejected going forward); WebRTC star with inner E2E (deferred — second SFU shape); full video on libp2p before voice green (rejected — scope).

**Dogfood claimed (2026-08-02):** Android ↔ Android LAN 1:1 bidirectional voice on libp2p `call-media` (moto g7 play ↔ Samsung SM-T380). Desktop matrix and NAT/hop not claimed. Details: [CURRENT_STATE.md](CURRENT_STATE.md).

**Cross-link:** [media-hop-reachability](../media-hop-reachability/) (dialability); mesh N018–N022.

---

## V027 — Mobile call-scoped listen on Wi‑Fi

**Date:** 2026-08-01  
**Status:** Accepted (**implemented** — see mesh N025 / `MessagingHub::SyncMobileEphemeralListen`)  
**Decision:** Mobile does **not** become a full mesh **Node**. During an **active foreground call on Wi‑Fi**, the app may **listen ephemerally** and publish dialable addrs (mesh **N025**) so that:

| Scenario | Benefit |
|----------|---------|
| **LAN 1:1 libp2p (m1)** | Callee reachable **by PeerId** without pasted multiaddr or desktop in the middle |
| **LAN group (N≥3)** | Phone on the call may act as **in-call `media_relay` hop** (`PreferInCallMediaHops`) when policy allows |
| **Address book** | Identify ads populate peerstore for contacts — helps **L4** PeerId-only messaging |

**Does not replace:** **`call_wake`** for incoming when app is background/killed (V006). **Does not replace:** org seed / desktop hop on **cellular** or when not in an eligible call session (V008 still applies off-LAN).

**Optional later:** user opt-in **Help on Wi‑Fi** (N025 mode 3) outside a call — separate from call-scoped listen; strict caps.

**Gating (product):** Wi‑Fi detected + foreground call (or explicit Wi‑Fi helper); **contacts-only** relay admission on mobile; no public/paid relay surface on phone.

**Rationale:** Client-only mobile blocked LAN PeerId call goals; full Node wrong for OS/battery. Call-scoped listen aligns with when the user already expects realtime media and the app is foreground.  
**Alternatives:** Require desktop/seed for all mobile LAN (rejected for UX); full mobile Node (rejected); ICE-only LAN forever (superseded by V026).  
**Mesh ADR:** [N025](../p2p-mesh/DECISIONS.md#n025--mobile-call-scoped-listen-on-wi-fi-not-full-node). **Phase:** [m1](PHASES.md#m1--libp2p-only-voice-v026) + mesh [nm](../p2p-mesh/PHASES.md#nm--mobile-call-scoped-listen-n025).

**Dogfood:** Mobile LAN 1:1 path exercised with call-scoped listen (2026-08-02) — see V026 dogfood note / [CURRENT_STATE.md](CURRENT_STATE.md).

---

## V028 — Call-scoped media_relay admission + prefer owner hop

**Date:** 2026-08-03  
**Status:** Accepted (**implemented**)  
**Decision:** For N≥3 SoftMigrate:

1. **Owner picks the hop** (sticky initiator SoftMigrate, unchanged V021/V022).
2. **Admission:** the first dialer (or `AttachAsLocalHop`) that opens a `media_relay` session for `call_id` must pass normal contact/scope admission. **After** that session exists, further attaches to the same `call_id` are admitted even if the dialer is not in the hop’s contact set — including mid-call stranger joiners. Mobile remains non-Public for *new* sessions (V027); only join-to-existing-call is the exception.
3. **Ranking:** when the initiator is a **durable Node** with `media_relay` started, prefer **self** as hop (`PreferLocalMediaHop` → `AttachAsLocalHop`) ahead of PreferInCall phones / seeds. **Not** for mobile ephemeral `media_relay` (V027) — phones must not SoftMigrate themselves into the SFU host role.

**Rationale:** Guests need not be mutual contacts of each other or of an in-call phone hop. Owner-sponsored call semantics match signaling (owner can reach each invitee). PreferLocal keeps the owner **Node** as the default host when available; ephemeral mobile Start() alone must not claim PreferLocal (dogfood: Android hop crash → peer `Connection reset`).

**Alternatives:** Require mutual contacts among all participants (rejected — UX); open mobile Public for new sessions (rejected — V027); signed owner attach tokens (deferred — same product rule, stronger crypto later).

## V029 — Durable hop rank + guest hop hints

**Date:** 2026-08-03  
**Status:** Accepted (**implemented**)  
**Decision:** For N≥3 SoftMigrate / attach:

1. **Owner picks + broadcasts** (`CallSfuAttach`) remains the sole attach authority (V021/V028).
2. **Ranking:** PreferLocal only for **durable Node** (`prefer_local_as_hop`). Do **not** PreferInCall phones as SFU host (amends V027 “in-call hop” for group SFU). Then ranked contact∪seed hops.
3. **Guest attach failure:** guest sends `call_sfu_attach_failed` with ordered `preferred_hop_peer_ids` (dialable hops, capped). Owner intersects with its dialable rank (excluding the failed hop):
   - Intersection non-empty → SoftMigrate re-pick (preferred hop first) + new `CallSfuAttach` fan-out.
   - Empty → `call_hop_refuse` to guest + eject; friendly copy for guest and owner toast.
4. **Mid-call add/remove:** same SoftMigrate / hint / refuse loop when hop must change.

**Rationale:** Owner cannot know guest↛hop a priori; guest prefs turn attach failure into switch-or-refuse instead of blind thrash or silent leave. Phones must not PreferLocal/PreferInCall into the SFU host role (dogfood crash).

**Cross-link:** V023 / V027 / V028; [CALLS.md](../../docs/architecture/CALLS.md) media_relay.

---

## V030 — Call capability ads (`caps`) for SoftMigrate hop eligibility

**Date:** 2026-08-03  
**Status:** Accepted (**implemented**)  
**Decision:** SoftMigrate must not treat “dialable listen addr” as “hosts `media_relay`.”

1. **Wire (additive):** `call_invite` / `call_accept` may include `"caps": { "v": 1, "media_relay": bool }`. Old peers ignore unknown keys. Missing `caps` = old peer.
2. **Advertise `media_relay` only** when durable **Node** + capability + service started. Ephemeral mobile listen-only must send `media_relay: false` (or omit hosting).
3. **Ranking:** PreferLocal (durable) + org seeds always eligible; **contact** hops only if cached `media_relay` ad is true (fail closed). PreferLocal self is prepended after this filter.
4. **Versioning:** bump `caps.v` only on semantic break; newer unknown `v` → treat as unusable for hop pick (do not fail invite parse).
5. **UX:** SoftMigrate / attach failures surface friendly copy (`call.error.no_media_relay_hop`, `call.error.local_media_relay_unavailable`) — not raw `media_relay not started`.
6. **Follow-up:** Identify capability ads (mesh PHASES ns) so hops are known before a call.

**Rationale:** Dogfood: tablets publish listen addrs (N025) and were wrongly SoftMigrated into SFU host → `media_relay not started` / Couldn't connect.

**Cross-link:** V029; [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md) additive wire; mesh [PHASES ns](../p2p-mesh/PHASES.md).

---

## V031 — Call chrome modes (Minimized / Expanded / Immersive)

**Date:** 2026-08-05  
**Status:** Accepted (implementing)  
**Decision:** In-call UI has **three modes** with clear jobs — not arbitrary window sizes.

| Mode | Job | Default |
|------|-----|---------|
| **Expanded** | Top call bar (+ stage when video); multitask under it | 1:1 on join |
| **Immersive** | Call is primary surface; full participant grid (voice avatars now, video tiles later) | Group / `show_roster` on join |
| **Minimized** | Ambient presence while using the app | Explicit collapse only |

### Mode ladder

```text
Immersive  ↔  Expanded  ↔  Minimized
```

One-step transitions only (no Immersive → Minimized in one fling). Restore from Minimized returns to the last non-minimized mode.

### Gestures (top-anchored chrome)

| Mode | Gesture | Result |
|------|---------|--------|
| **Expanded** | **Tap** non-button call chrome (bar / stage) | → Immersive |
| **Expanded** | **Tap** outside call chrome (not on modal overlays) | → Minimized |
| **Immersive** | Swipe / pull **down** on non-button chrome, **or** pull **down** starting in the people list when `scrollTop == 0` | → Expanded |
| **Minimized** | **Tap** chip | Restore last Expanded/Immersive |
| **Minimized** | **Drag** chip | Reposition; snap to corner |
| **Minimized** | Swipe | **None** (avoids fighting drag) |

**Hit-test model (Expanded):** the mount layer is `pointer-events: none` except the visible bar/stage; taps outside chrome reach the shell and dismiss to Minimized (click is consumed — chat controls underneath do not activate). Tap on chrome (not buttons) enters Immersive.

**Hit-test model (Immersive):** button → control; scroll region (when not at top / not pulling down) → scroll; everything else → mode swipe. Same idea as `ShellBottomSheetGesture` (ignore buttons; scroll-at-top handoff).

**Hard rules:** Ring stays modal. No gesture ends the call. Escape/back still does not hang up. Immersive keeps visible expand control mirroring pull-down dismiss. Mode changes remount `#shell-call-in-progress-mount` only (`RemountCallChrome`); re-attach gestures after remount.

**Rationale:** Multitasking needs a true minimize; group voice needs everyone visible — Expanded cannot honestly show 8–16 peers. Three jobs beat three decorative sizes. Pull-down to leave Immersive matches list overscroll-at-top and reuses sheet gesture patterns.

**Alternatives rejected:** Three free-floating resizable windows; pinch-to-mode-change; swipe-to-end; side-pad-only Immersive swipes as v1 layout; OS PiP / post-Present overlay (still V018).

**Cross-link:** V018 (in-shell tiles); V019 (unified in-call); [WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md); [DESIGN.md](DESIGN.md) UI sketch.

---

## V032 — Media QoS enforcement, playout, SFU E2E

**Date:** 2026-08-06  
**Status:** Accepted (implementing)  
**Decision:** Close the structural gaps between N019/N021/V024 **docs** and host/receiver **code** before further quality tuning. Policy matrix: [HOST_RECEIVE_POLICY.md](HOST_RECEIVE_POLICY.md).

| Area | Rule |
|------|------|
| **A↑ / A↓ enforcement** | Hop enforces with token buckets on Fanout ingress (publisher A↑) and egress (subscriber A↓). Sender also paces via Opus `target_audio_bps`. Quote fields are not decorative. |
| **B↑ / B↓ + ceiling** | Session totals soft-cap fan-out; hard stop when `bytes_total > ceiling_bytes` (unchanged intent, still enforced). |
| **Audio under pressure** | Never use LatestLossy skip-to-latest on audio. Extreme hop outbound backlog: **drop-oldest** (live preference). Receiver never skip-to-latest. |
| **Host load admission** | Max **4** concurrent `HostSession`s; max **8** participants per session (matches eng call cap). Refuse quote/attach over limit. |
| **Receiver delay** | Per-`stream_id` Opus decoder + jitter target **60 ms**, max **200 ms**, PLC on gap; mix to one playback device. Hop stays blind (no jitter buffering on hop). |
| **Adaptation actuators** | `path_pressure` (playout underruns / hop drops) drives Opus bitrate within V024 ladder (`kMinAudioBps`…`kDefaultAudioBps`) and continues to gate video. |
| **SFU E2E** | Encrypt/decrypt SFU payloads with the call media key (same AEAD family as 1:1). AAD includes `stream_id`. Hop never holds keys. |
| **1:1** | Unchanged wire; already AEAD. Shares engine playout path (`stream_id` 0). |

**Rationale:** Group voice degraded without per-stream decode/playout and without real ↑/↓ enforcement — budgets and adaptation existed as schema/policy only. Structure must land before constant-tuning the “goes silent” dogfood bug.

**Alternatives rejected:** MCU mix on hop (breaks blindness); hop-side jitter buffer (wrong layer); shared single OpusDecoder for all publishers; defer SFU E2E indefinitely.

**Cross-link:** V022 / V024; mesh N019 / N021; [CALLS.md](../../docs/architecture/CALLS.md); [HOST_RECEIVE_POLICY.md](HOST_RECEIVE_POLICY.md).

---

## V033 — Transport session machines (not host-wide inbound SM)

**Date:** 2026-08-07  
**Status:** Accepted (**s2a done** — call-media phases in code; media-relay N026 **s3a+s3b done**; circuit compose loopbacks green)  
**Decision:** Introduce explicit **flat enum + `Apply(event)`** state machines for **long-lived** mesh host media sessions — first **`CallMediaDirectService`** (one active 1:1 session), then **`MediaRelayService`** per-inbound-stream attach ([N026](../p2p-mesh/DECISIONS.md#n026--media_relay-per-stream-attach-state-machine)). Do **not** build a host-wide “incoming request” state machine. Do **not** rewrite one-shot RPCs (chat, chat-history, dial-back) as SMs. Do **not** move product `CallPhase` / `CallLifecycle` into `base/p2p`.

| Rule | Detail |
|------|--------|
| **Style** | Mirror `CallLifecycle` — phases, events, `Apply`, INFO `phase=` / `event=` logs. No SM framework. |
| **Layer** | Transport SM in `CallMediaDirectService` (integration/host). Product ring/accept/InCall stays in `CallLifecycle`. Bridge/Topology decide *when* to Connect/Detach; SM owns stream session legality. |
| **Threading** | Handler on host io → control handshake on worker **Normal** (never Critical). `Apply` on one strand per service. Duplex R/W on host io. Waiters/timeouts owned by the SM. |
| **Migration** | Docs → freeze open questions → strangler one service at a time → loopback goldens. No wire bump required. No drive-by chat/history refactors in the same PR. |
| **Spec** | [SESSION_MACHINES.md](SESSION_MACHINES.md) |

**Rationale:** m1 call-media works but is held together by flags (`outbound_hello_inflight`, `settled` atomics) and comments (glare, SoftMigrate EOF). That knowledge belongs in an explicit machine before more patches. A blanket host inbound SM would add ceremony to simple RPCs and blur protocol differences.

**Alternatives rejected:** Host-wide inbound SM; hierarchical/Harel frameworks; absorbing CallLifecycle into the host; big-bang rewrite of MessagingHub; SM-ifying chat/dial-back.

**Cross-link:** [CALLS.md](../../docs/architecture/CALLS.md) (CallLifecycle + critical races); [HOST_RECEIVE_POLICY.md](HOST_RECEIVE_POLICY.md); mesh [N026](../p2p-mesh/DECISIONS.md#n026--media_relay-per-stream-attach-state-machine); [THREADING.md](../../docs/architecture/THREADING.md).

---

## V034 — Video on libp2p (video_lo; E2E; same streams)

**Date:** 2026-08-18  
**Status:** Accepted (**implementing**)  
**Decision:** Product **video** rides the existing libp2p voice path — not WebRTC. Capture/encode/decode/tiles stay the a3 SDL + platform HW H264 stack ([V017](#v017--video-codec-h264-via-platform-hw) / [V018](#v018--video-capture--render-path-in-sdl--rmlui-shell)). Transport is **encrypted H264 video_lo** on the same 1:1 duplex and the same blind `media_relay` hop as Opus.

| Topic | Rule |
|-------|------|
| **Codec** | H264 Constrained Baseline Annex-B via `IVideoCodec` (~640×360 desktop / ~360×640 mobile after orientation @ ~20 fps). |
| **Layer** | **video_lo only.** `allow_video_hi` stays false; simulcast / `video_hi` is a later horizon. |
| **1:1** | Same `/pp-browser/call-media/1.0.0` duplex as audio. Frame **v2**: `ver=2 \| seq \| mark \| channel \| nonce \| ct`. Channel `0` = Opus, `1` = H264 AU. Decrypt **v1** bodies as channel 0 (voice interop). Cap **128 KiB** (no NAL fragmentation in this slice). |
| **E2E / uplink** | **One key per call epoch** ([V004](#v004--shared-call-media-key-not-group-n-ciphertext)), not per subscriber. Publisher AEAD-seals each AU **once**; hop copies that ciphertext to subscribers. Per-target encrypt is rejected — it would multiply video uplink by N−1. AAD `stream_id` + channel is replay binding, not a per-peer key. |
| **Group / SFU** | Existing N021 `channel_id=1` + `LatestLossy` + `mark=1` on IDR. Hop **never** inspects H264. Same shared-key AEAD on **all** channels as audio. |
| **Hop queues** | Never shed `ReliableOrdered` (audio) to enqueue `LatestLossy` (video). Drop stale video first. |
| **Camera** | Off on join ([V009](#v009--any-group-member-may-start-camera-off-by-default-on-join)). Adaptation `camera_user_wants` follows `IsCameraEnabled()`. Missing encoder/decoder must not tear down voice ([V019](#v019--unified-call-media-shape-voicevideo-entry-only)). |
| **IDR** | Encoder `force_keyframe` on start / SoftMigrate send-swap / inbound `call_video_refresh`. Periodic IDR remains a backstop. |
| **Group UI** | Expanded: one remote stage + local PiP. Immersive: per-peer tiles (cap concurrent HW decoders, e.g. 4). |
| **Non-goals** | `video_hi`, screen share, recording, CallKit, libdatachannel, Linux soft-codec, N021 header change. |

**Updates:** [V026](#v026--libp2p-only-call-media-http--libp2p-networking) “video deferred” — video_lo is now in scope after voice-on-libp2p. [V024](#v024--adaptive-call-media-11-p2p-and-sfu-generic-relay-channels) single-layer mapping unchanged.

**Rationale:** Voice path, hop framing, and camera stack already exist. The gap is encrypted send/receive + hop audio-priority queues + per-stream decode — not a second media stack. Group video stays **one seal per AU** so camera uplink stays a single stream regardless of roster size.

**Cross-link:** [CALLS.md](../../docs/architecture/CALLS.md); [HOST_RECEIVE_POLICY.md](HOST_RECEIVE_POLICY.md); [PHASES.md](PHASES.md) **lv** (libp2p video).

---
