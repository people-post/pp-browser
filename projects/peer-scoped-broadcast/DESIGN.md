# Peer-scoped announce + live broadcast — design

**Status:** Accepted sketch (2026-09-05)  
**Implements:** nothing yet — memory aid for a later phase  
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
- **No public in-topic reply mesh.**
- To respond: send a **direct message** to the publisher. If they want it “public,” **they** rebroadcast under their topic (their rate limits / moderation).

### Payload policy

- **Small announcements only** (text, pointers, caps, schedule/live tips).
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

- **Watch** → realtime subscriber path (SFU/hop).
- **Chat / react “publicly”** → DM (or side chat) to publisher; publisher may rebroadcast a text announce — still not free-speak on the media mesh.

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
2. **Go live** → announce `state=live` + join params → viewers attach realtime; `help_media` peers may hop.  
3. **End** → announce `state=ended` + optional `content_id` for DVR; tear down realtime.  
4. **Revoke** → stop signing; rotate topic and/or invalidate join tokens; hops detach.

---

## Explicit non-goals

- Open topics anyone can create or speak on  
- Classic GossipSub as the product path  
- Public threaded replies on the announce plane  
- Carrying live video (or large blobs) on announce/gossip  
- Replacing chat DM, mesh DHT `FIND_PEER`, or content-cas provide/fetch  
- Minting a new L4 kind such as `/pp-browser/broadcast/…` (use rpc + realtime + blob)

---

## One-line summary

**Authenticated per-PeerId announcement feeds, optionally relayed by whitelisted helpers; live picture on realtime hop; optional recording in CAS; conversation stays DM; the publisher alone controls what becomes public again — one helper relationship, explicit `help_announce` / `help_media` flags.**
