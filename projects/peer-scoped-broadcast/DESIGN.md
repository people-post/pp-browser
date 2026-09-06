# Peer-scoped announce + live broadcast — design

**Status:** Accepted sketch (2026-09-05); pickup UX + overlay reply product rules amended same day  
**Implements:** Spine B tips + Spine C join handoff in code — see [CURRENT_STATE.md](CURRENT_STATE.md); Notifications/banner and overlay path still design-only  
**Program sequencing:** [PROGRAM.md](PROGRAM.md) (spines A–E across mesh / calls / CAS)  
**L4 fit:** announce ≈ **rpc** (+ optional small relay); live A/V ≈ **realtime** blind hop; DVR/replay ≈ **blob** / [content-cas](../content-cas/)

## Problem

We want:

1. A way for a peer to **broadcast small announcements** under their identity.
2. Optional peers to **help distribute** those announcements.
3. **Live video broadcast** to many viewers without inventing a new L4 kind.
4. A clear story for **replies** that does not turn gossip into an open spam mesh.

Open GossipSub-style topics fail our constraints: anyone can create/speak on a name, amplification DoS is easy, collisions/squatting are real, and topic lifecycle is emergent with no owner.

## Solution — two planes, one helper relationship

```text
┌─────────────────────────────────────────────────────────┐
│ Announce plane (small, signed, sparse)                  │
│  Publisher PeerId owns topics under itself              │
│  Helpers may forward if whitelisted (help_announce)     │
└──────────────────────────┬──────────────────────────────┘
                           │ points at join / program id
                           ▼
┌─────────────────────────────────────────────────────────┐
│ Live media plane (lossy, paced, multi-receiver)         │
│  realtime blind hop / media_relay (existing calls path) │
│  Helpers may hop if whitelisted (help_media)            │
└──────────────────────────┬──────────────────────────────┘
                           │ optional ended → content_id
                           ▼
┌─────────────────────────────────────────────────────────┐
│ Durable plane (optional DVR / VOD)                      │
│  content-cas blob / public or private object            │
└─────────────────────────────────────────────────────────┘
```

Do **not** carry video bitstreams on the announce/gossip plane.

---

## Announce plane (peer-scoped feed)

### Roles

| Role | Who | May |
|------|-----|-----|
| **Publisher** | A PeerId | Define topics under itself; **sole author** of posts on those topics; decide whether a DM becomes a public rebroadcast |
| **Announce helper** | Any peer | Opt in to **forward** selected `(peer_id, topic_id)` posts |
| **Reader** | Any peer | Subscribe to receive without necessarily forwarding |

**Subscribe-to-read** and **help-forward** are separate switches.

### Topic model

- Topics are **namespaced by PeerId** — no global free-form topic creation.
- Stable wire id, collision-resistant, e.g.  
  `topic_id = H(peer_id ‖ app_ns ‖ local_name)`  
  (or an explicit `pp-browser/peer/<peer_id>/<topic_id>` label in the envelope).
- Publisher’s local names are UI-only; others treat `topic_id` as opaque.

### Speak / reply rules

- Helpers **must drop** anything not **signed by that PeerId**, with `topic_id` bound into the signed payload.
- **No public in-topic reply mesh.** Viewers never speak on the announce topic or on the media hop.
- Subscriber reply modes (product):

| Mode | Transport | Who sees it |
|------|-----------|-------------|
| **Private** | Normal DM to the publisher | Publisher only |
| **On screen** (live overlay) | Still a request **to the publisher** (DM or small rpc), tagged `intent=overlay` + live session id | Everyone — **only after** the publisher signs a short announce tip (e.g. `kind=live_chat`) that helpers may forward |

```text
Viewer "Send to live"
  → DM/rpc to publisher: text + live_session_id + intent=overlay
Publisher device (policy)
  → rate-limit / block / allow
  → if ok: sign overlay tip → helpers forward → viewers render on live UI
```

Publisher remains sole author on their topic. Busy-while-live is handled by **device policy**, not by opening the mesh:

| Publisher mode | Behavior |
|----------------|----------|
| **Auto** (default while live) | Allow-list / score / rate limit → auto-sign overlay tips |
| **Moderated** | Queue; Approve / Block in publisher live chrome |
| **Off** | Overlay intent falls back to private DM only |

**Abuse controls for overlay (minimum):**

- Per-viewer rate toward that live session (e.g. 1 overlay request / N seconds).
- Publisher outbound cap on signed overlay tips / minute (same budget family as announce, **not** media).
- Short text only; no blobs on the announce plane.
- Publisher block / mute drops future overlay intents; optional grey-list for repeat spam.
- Session bind: overlay tips carry `join_handle` / program id so they do not leak across shows.
- Dedup: same `(publisher, session, viewer_msg_id)` once.
- Helpers forward **publisher-signed** overlay tips only — never raw viewer speech.

### Payload policy

- **Small announcements only** (text, pointers, caps, schedule/live tips, optional `live_chat` overlay tips).
- Large bytes stay on **CAS / blob provide-fetch** (put a content id in the announce).

### Lifecycle

| Event | Meaning |
|-------|---------|
| Create | Publisher starts signing posts for a `topic_id` |
| Join | Local subscribe and/or helper whitelist — no global registry |
| Leave | Drop subscribe/whitelist; stop forwarding |
| End / revoke | Publisher stops posting; optionally **rotate** `topic_id`. Old ids are abandoned, not globally deleted |

Existence is **emergent** (use + interest), not admin CRUD.

### Abuse controls (non-negotiable)

- Signed posts; topic id in the signature binding.
- Per-`(publisher, topic)` **size + rate limits** on helpers and on publisher rebroadcast-of-DMs.
- Peer scoring / grey-list for forward abuse.
- Prefer **IHAVE/IWANT-style pull** for anything non-tiny; blind push only for small envelopes.

---

## Live video plane

Matches existing L4 composition ([L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md)):

> Live broadcast = **rpc** (catalog / subscribe / token) + **realtime** blind hop (fan-out); optional **blob** for DVR/VOD.

### Publisher flow

1. Sign an announce on `(peer_id, topic_id)` (e.g. `live` / show id): title, state, join/session handle, capability hints.
2. Announce helpers may forward that **small** tip (`help_announce`).
3. Viewers attach to **realtime** hop with normal broadcast/call attach rules.
4. Media frames never ride the announce plane.

### Viewer / chat

- **Watch** → realtime subscriber path (SFU/hop); product entry is Notifications / banner → join API (see [Product pickup UX](#product-pickup-ux--not-call-ringing)), not call ringing.
- **Private reply** → DM to publisher (existing messaging path).
- **On-screen reply** → overlay request to publisher → publisher-signed tip → live overlay / chat rail (see [Speak / reply rules](#speak--reply-rules)). Still not free-speak on the media mesh.

### Live re-announce (heartbeat)

While a program is **live**, the publisher may **auto-repeat** a small signed tip so late helpers/readers still discover the session. This is **publisher-driven refresh**, not helper-invented spam.

| Rule | Policy |
|------|--------|
| Who emits | **Publisher PeerId only**; helpers forward, they do not set their own cadence |
| When | Only while the realtime session is live; stop on end/revoke. Optional separate “schedule/presence” mode is out of default live path |
| Minimum interval | Floor **≥30–60s** between live heartbeats; also enforce a **max** (e.g. ≤1/min) |
| Jitter | Prefer **jittered / desynchronized** schedule over a fixed metronome (spreads helper load) |
| Payload | **Heartbeat ≠ full blurb**: `state=live`, `program_id`, `seq`/`epoch`, join/session id or token hash; keep title/description on the go-live tip |
| Dedup | Helpers/readers drop if same `(peer_id, topic_id, program_id, epoch)` (or lower `seq`) seen inside the interval window — stops mesh echo amplification |
| Budget | Counts against **`help_announce`** rate limits; must **not** track video bitrate or `help_media` cost |
| Pull | After first tip, prefer **IHAVE / IWANT**-style pull for quiet meshes; rare push OK for cold start |
| Events that bypass the floor once | Go-live, SoftMigrate / token rotate, end (`state=ended` + optional DVR `content_id`) |

**Intent:** improve mid-show audience reach via discovery tips, not via announce-plane video or unbounded beaconing.

---

## Product pickup UX — not call ringing

Discovery tips are **subscription events**, not mutual call invites. Many subscribed publishers may go live; urgency is lower than a 1:1/group ring.

| Concern | Owner | Product surface |
|---------|--------|-----------------|
| Tip arrival / schedule / live / end | Announce feed | **Notifications** tab (subscribed messages) |
| Soft interrupt while live | Same tip, not dismissed | Optional **sticky banner** (“X is live”) → Watch / Dismiss |
| Watch / join media | Calls stack (reuse hop) | Join API under the hood; **no** Accept/Decline ringtone |
| Private reply | Messaging | Opens/finds DM thread |
| On-screen reply | Messaging request + announce rebroadcast | Live overlay after publisher signs |
| Publish / go live / end | Publisher chrome | Me / composer — **not** the Notifications tab |

```text
Subscribed tip arrives
  → upsert Notifications item (by publisher + topic + epoch/seq)
  → if live + not dismissed → show/update banner (heartbeat refreshes same card)
User taps Watch
  → JoinLiveAnnounceFromTip / AcceptLiveAnnounceJoin (media handoff; no ring UI)
User taps Reply
  → Private DM  or  On-screen overlay intent (publisher policy)
```

**Spine C media handoff** (`ArmJoinFromLiveAnnounce`, pending invite shapes) may reuse call **session** types for SFU attach. That is an implementation convenience — **do not** present announce join as incoming-call chrome (`NotifyRingChanged`, ringtone, bilateral `CallAccept`). Prefer a tip list + banner that calls the same accept path.

Tips stay on the announce feed (in-memory today); they are **not** chat rows in `SqliteThreadStore` unless a later durable-feed spine deliberately stores history. OS push, if any, should use a quiet notify type — not `call_wake`.

---

## Shared helper whitelist (product)

Helpers use **one relationship** to an announcer (“I support PeerId X”), with **capability flags**:

| Flag | Volunteers |
|------|------------|
| `help_announce` | Forward their signed small posts on allowed topics |
| `help_media` | Act as realtime hop/SFU for their live sessions (**much costlier**) |

### Rules of thumb

- Same **mental model** and UI allowlist of PeerIds (or programs).
- Still **two planes** under the hood (announce forward ≠ media hop).
- Default should be safe: e.g. **announce-only** unless the user explicitly enables `help_media`.
- Media hop keeps existing A↑/A↓ / attach-token / quota policy from calls/mesh — whitelist is admission of *who*, not a blank check for unlimited HD fan-out.

**Readers of announces ≠ announce helpers ≠ media hops ≠ media subscribers** — four knobs, even if UI collapses the first three into “support this creator.”

---

## Combined lifecycle (a live show)

1. **Tease / schedule** → announce only.  
2. **Go live** → fuller announce `state=live` + join params → viewers attach realtime; `help_media` peers may hop.  
3. **While live** → publisher auto-repeats **heartbeat** tips on the min-interval / jitter policy above; helpers dedup and forward under `help_announce`.  
4. **End** → one `state=ended` tip (+ optional `content_id` for DVR); stop heartbeats; tear down realtime.  
5. **Revoke** → stop signing; rotate topic and/or invalidate join tokens; hops detach.

---

## Media scale (multi-hop tree)

Single-hop `media_relay` (Spine C) is enough for small audiences. Massive subscriber counts need a **degree-capped tree of blind SFUs** so seed / PreferLocal root egress stays ~`degree × bitrate`. Plan: **[MEDIA_TREE.md](MEDIA_TREE.md)** (ADRs [B001–B007](DECISIONS.md), phases B0–B3, program **Spine F**).

**Audience finds capacity via a recursive whitelist ladder ([B007](DECISIONS.md#b007--recursive-whitelist-ladder-discovery-admit-or-redirect)):** tip lists only the publisher’s **online L1** `help_media` PeerIds; each hop **admits** if it has a free viewer slot, else **redirects** to its own whitelist∩online children; a new whitelist relay may **win a slot** and push piped viewers one rung down. Publisher mints the media ticket (key); hops do not need a global leaf map.

Summary locked there:

- Broadcast ≠ large group call (do not SoftMigrate / raise V007).
- Keep encrypt-once AEAD; hops stay blind; stable session key + join ticket (no rotate-on-viewer-leave).
- **B007:** tip names online L1 whitelist only; hops admit-or-redirect; slot-win demotion pushes piped viewers down.
- Viewers settle where capacity exists; relays are `help_media` Nodes.
- Circuit multi-hop remains reachability only; media copies only at `media_relay` nodes.
- Group calls keep one-hop SFU; multi-SFU media is **broadcast-only**.

---

## Explicit non-goals

- Open topics anyone can create or speak on  
- Classic GossipSub as the product path  
- Public threaded replies or free-speak on the announce / media planes (overlay only via publisher-signed tips)  
- Presenting live-tip pickup as call ringing / `call_wake`  
- Carrying live video (or large blobs) on announce/gossip  
- Putting live chat bitstreams on realtime media frames  
- Replacing chat DM, mesh DHT `FIND_PEER`, or content-cas provide/fetch  
- Minting a new L4 kind such as `/pp-browser/broadcast/…` (use rpc + realtime + blob)  
- Multi-SFU media trees for **group calls** (broadcast tree is [MEDIA_TREE.md](MEDIA_TREE.md) / Spine F — not a call SoftMigrate extension)

---

## One-line summary

**Authenticated per-PeerId announcement feeds, optionally relayed by whitelisted helpers; discovery via Notifications + optional live banner (not call ring); while live, publisher-paced heartbeat tips for late discovery; live picture on realtime hop (scale via degree-capped blind SFU tree); private DM or publisher-mediated on-screen overlay replies; optional recording in CAS; one helper relationship, explicit `help_announce` / `help_media` flags.**
