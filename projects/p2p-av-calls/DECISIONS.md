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

---

## V016 — a3 delivery slice: LAN video; mobile wiring included

**Date:** 2026-07-28 (updated 2026-07-30 — LAN dogfood claimed; a3 closed)  
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

**Dogfood claimed (2026-07-30):** Android ↔ Windows bidirectional video; Android→Linux and Windows→Linux one-way (Linux dogfood host had no camera; receive/display OK). macOS / iOS device optional. NAT / seed SFU not claimed. 

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

**Linux constraint (accepted):** Video **send** requires a capture device **and** a usable H264 encoder. Many Linux hosts (VMs, headless, missing iGPU drivers, no camera) fail one or both. a3 dogfood Linux had **no camera** (receive OK from Android/Windows). Do **not** block a3 on universal Linux soft encode. Receiving/decoding may still work when a HW decoder exists.

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
| **B. Custom `<call-video-tile>` element `OnRender` + app-owned persistent GL tex** (`glTexSubImage2D`, letterbox `RenderGeometry`) | **Adopt** — paints in document stacking (below banner/dialogs); app keeps texture ownership; DirtyWindow only |
| **C. OpenGL blit after `Context::Render` / `PresentFrame`** | Reject — breaks stacking (video over banner), hit-testing, safe-area |
| **D. Remount shell when video appears** | **Forbidden** — agent trap: use `data-if` + `DirtyWindow` only ([WINDOW_SHELL](../../docs/ui/WINDOW_SHELL.md)) |
| **E. `GenerateTexture` every frame** (full reallocate) | Reject for steady state — GC/alloc cost at 15–30 fps |

**UI composition (1:1 a3; V019 — same in-call for Voice/Video start):**

1. In-call chrome is unified once connected: icon mute / camera / leave + meters / elapsed (allowed on voice-started calls too). Compact layout uses a stacked bar so controls stay on-screen.  
2. When remote (or local) video is active, expand an in-shell **stage** (still overlay, not a new nav tab): large **remote** tile; small **local PiP** when local camera on; placeholder / avatar when remote camera off. Compact bar-only when neither side has video frames.  
3. Camera toggle off → on requests permission + opens SDL camera (encode starts then).  
4. Chrome gate remains `CallChromeSync` / `DirtyWindow` — frame pixels update without remounting; only layer identity changes remount-class dirty.

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
| SFU hosts | Org **`pp-node`** + desktop **`media_relay`** (blind; volunteer **default on** — N018 / V021). Mobile never hosts. |
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
| **Hosts** | Org seed + desktop Node; capability **default volunteer on** when Node is on (N018). Mobile never hosts. |

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
**Decision:** Hop selection for blind `media_relay` is a **risk-aware scorer over eligibility classes**, not a hardcoded contacts→seed→public list. **Making money is not the product goal**; the **pricing model exists to regulate** scarcity, strangers, and abuse over time. Mesh twin: [N020](../p2p-mesh/DECISIONS.md#n020--media-hop-pick-short-term-closed-set-pricing-as-regulation).

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
