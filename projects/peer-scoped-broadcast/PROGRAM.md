# Peer-scoped broadcast — program board

**Status:** Planning accepted (2026-09-05) — guides multi-project sequencing  
**Product design:** [DESIGN.md](DESIGN.md)  
**Rule:** Optimize **program throughput** (cross-project dogfood slices), not single-project completion %.

## Operating model

**One integration spine, several parallel ribs.**

| Term | Meaning |
|------|---------|
| **Spine** | Next demoable path that needs ≥2 projects. Only the active spine(s) get merge priority. |
| **Rib** | Parallel work that clearly feeds the current spine or the immediate next one. |
| **Parking lot** | Good ideas that do not feed the current spine (defer). |

If a task cannot name which **spine exit** it advances, it waits.

**WIP limit:** at most **two** active spines (current + prep-next). A third track is docs-only.

**Cadence:** each cycle hits **one spine exit** + **≤2 ribs**. Demo is cross-project (e.g. tip → watch), not “CAS tests greener.”

**Contracts early, mesh late:** lock tip schema / heartbeat / dedup before epidemic `help_announce`. Reuse call realtime hop; do **not** mint a new L4 kind.

### Artifact owners (no duplicates)

| Artifact | Owner project |
|----------|----------------|
| Tip / announce envelope, heartbeat, topic ids | **peer-scoped-broadcast** |
| Media attach / broadcast-shaped session | **p2p-av-calls** |
| Hop admit, capabilities, helper whitelist policy | **p2p-mesh** |
| Durable object id / provide-fetch / DVR object | **content-cas** |

---

## Spines (in order)

### Spine A — Calls hop is trustworthy

| | |
|--|--|
| **Owners** | [p2p-av-calls](../p2p-av-calls/), [p2p-mesh](../p2p-mesh/), [media-hop-reachability](../media-hop-reachability/) |
| **Exit** | Group / N≥3 SoftMigrate + `media_relay` dogfood stable; attach tokens + A↑/A↓ quotas understood |
| **Why first** | `help_media` and live broadcast reuse this path |

**Ribs (OK in parallel):** mesh hop-policy / capability-ad cleanup (no announce protocol); calls session SM (V033) → code; host receive policy hardening.  
**content-cas:** freeze features — bugfix / P2 fallout only.

### Spine B — Signed tips without a mesh

| | |
|--|--|
| **Owners** | **peer-scoped-broadcast** (thin code) + messaging/rpc |
| **Exit** | Publisher posts signed `(peer_id, topic_id)` tips locally + 1:1/small rpc to contacts; heartbeat while live flag on; DM reply path; **no** epidemic helpers |
| **Why next** | Locks wire/schema cheaply; dogfood discovery without amplification |

**Ribs:** calls keep SFU green; optional **broadcast-shaped session** behind a flag (invite-only, still call-like).  
**content-cas:** design-only for end tip → `content_id`; private DVR experiment only if calls need local replay.

### Spine C — Live program = tip + realtime

| | |
|--|--|
| **Owners** | peer-scoped-broadcast + p2p-av-calls |
| **Exit** | Go-live announce → viewers join realtime hop → heartbeat while live → end tip. Contact/seed hop only (not open helper market) |
| **Why** | First real vertical of [DESIGN.md](DESIGN.md) |

**Ribs:** mesh `help_media` as explicit capability on existing relay pick (PeerId whitelist), narrow.  
**content-cas:** start **P3 thin** only if end tip needs a durable object story.

### Spine D — Helpers amplify tips

| | |
|--|--|
| **Owners** | peer-scoped-broadcast + p2p-mesh |
| **Exit** | `help_announce` whitelist, dedup, rate limits, jittered heartbeat forward |
| **Why after C** | Schema + live semantics already proven |

### Spine E — Public replay / library

| | |
|--|--|
| **Owners** | [content-cas](../content-cas/) P3/P4 (+ broadcast end tip) |
| **Exit** | End announce can point at CAS object; provide/fetch or CDN path works |
| **Why last** | Hardest durable/public surface; must not block live dogfood |

**C012:** peel to `domain/content` only when Spine E (or library) is a **second** CAS owner outside messaging.

---

## Parking lot (explicitly not spines yet)

- Creator helper marketplace / paid announce  
- Open GossipSub / free-form topics  
- Piece swarm (content-cas P5)  
- `domain/content` peel before Spine E has a second owner  
- Directory/DHT **content** provider routing (mesh DHT stays FIND_PEER-oriented for now)

---

## Parallel staffing sketch

| Track | Now (Spine A) | Next (B → C) |
|-------|----------------|--------------|
| Calls / mesh | SFU / SoftMigrate / quotas | Broadcast session flag + join from tip |
| Announce | Docs / envelope draft only | Local + rpc tips + heartbeat |
| CAS | Feature freeze | P3 only when end-tip needs it |
| Mesh policy | Capability clarity | `help_media` whitelist experiment after C |

---

## Status board

| Spine | Status | Notes |
|-------|--------|-------|
| A — hop trustworthy | **Still prerequisite** (parallel) | Track in p2p-av-calls / p2p-mesh CURRENT_STATE |
| B — tips without mesh | **In progress** — ML-DSA codec/feed/publisher/rpc codec; Amp 1:1 fan-out next | `PeerAnnounce*` under domain/messaging |
| C — tip + live | Blocked on A + B | |
| D — announce helpers | Blocked on C | |
| E — CAS replay | Blocked on C (product); P3/P4 not started | |

Update this table when a spine **exits**; link dogfood evidence in the owning projects’ CURRENT_STATE.

---

## One-line program summary

**A (hop) → B (tips) → C (tip+live) → D (announce helpers) → E (CAS replay)** — finish shared exits, not isolated project bars.
