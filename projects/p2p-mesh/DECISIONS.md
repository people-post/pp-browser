# P2P mesh — decisions

## N001 — Desktop node default / mobile always client

**Date:** 2026-07-26  
**Decision:** Exactly two **roles**: **client** and **node**. Desktop effective role is **node** when `node_enabled` is true (default). Mobile is always **client**; `node_enabled` is ignored on mobile.  
**Rationale:** Desktops can host infrastructure; mobiles must stay outbound-only for battery and OS limits.

## N002 — Seed multiaddr IP + 443 + PeerId (no DNS)

**Date:** 2026-07-26  
**Decision:** Fixed bootstrap multiaddr is `/ip4/3.208.41.58/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR`. IP only — no DNS multiaddrs in v1.  
**Rationale:** Deterministic Brief seed without DNS resolution failures; PeerId pins identity.

## N003 — Desktop listen 18517; seed on 443

**Date:** 2026-07-26  
**Updated:** 2026-07-26 (default port **18517**; was 40123)  
**Decision:** Desktop / in-app Node preferred listen is `/ip4/0.0.0.0/tcp/18517`. Seed / org `pp-node` uses TCP **443**. Do not collapse desktop onto 443. Busy-port behavior is **N016**.  
**Rationale:** App-specific high port avoids IPFS **4001** and other common daemons; 18517 is the Brief desktop default. Seed 443 stays firewall-friendly for clients dialing out.

## N004 — Client = no host→listen

**Date:** 2026-07-26  
**Decision:** Effective client role must not call `host->listen` (no private bind workaround in n1).  
**Rationale:** Clear mobile power win and unambiguous role semantics; outbound dials only.

## N005 — Capability protocols deferred; no half-enabled service UI in n1

**Date:** 2026-07-26  
**Decision:** n1 ships role + bootstrap + listen + master Network toggle only. Ship each capability flag **and** its checkbox in the same phase the protocol works (n2+). Pricing UI ships with the first **billable** capability (N010). Do not expose inert checkboxes.  
**Rationale:** Avoid toggles that do nothing; circuit-relay is absent from the fork; economy features need real meters first.

## N006 — Do not use public IPFS bootstrap.libp2p.io

**Date:** 2026-07-26  
**Decision:** Bootstrap only from the Brief seed (and later our own peers). Never default to public IPFS bootstrap lists.  
**Rationale:** Privacy, control, and independence from the public IPFS network.

## N007 — TCP 443 on seed is libp2p transport (≠ HTTPS Brief API)

**Date:** 2026-07-26  
**Decision:** Seed `/tcp/443` is the **libp2p** transport port. It is distinct from HTTPS Brief API endpoints used for HTTP relay/directory/registration.  
**Rationale:** Firewall-friendly outbound for clients without conflating TLS HTTP with multiplexed libp2p.

## N008 — Node = ecosystem infrastructure (mutual help)

**Date:** 2026-07-26  
**Updated:** 2026-07-26 (monetization detail moved to N010)  
**Decision:** **Node** means hosting Brief/pp-browser **infrastructure** for other peers — not only inbound chat sockets. Capability surface includes (over time) DHT, circuit relay, message / audio / video relay, blockchain node, and optional paid-jobs marketplace. Capabilities attach to the **node** role; they do not create new roles. How nodes get paid is **N010**, not “a paid-jobs capability that stands in for settle.”  
**Rationale:** Cooperative mesh where desktops voluntarily supply capacity; clients stay light.

## N009 — Role + capability checkboxes (not capability-only, not binary-only forever)

**Date:** 2026-07-26  
**Decision:** Product model is **role + capability checkboxes**. Role (`node_enabled`) answers “do I host?” Capabilities answer “which services do I run?” while role is Node. UI: master **Help the network** toggle; nested checkboxes appear as each capability ships. Effective service *C* requires `role == Node && C_enabled`. Turning role off forces Client behavior regardless of stored capability flags. Rejected alternatives: (a) forever all-or-nothing Node with no per-service choice; (b) capability-only with no master role. Optional later: presets that set checkbox bundles — still the same model.  
**Rationale:** Users need fine control (e.g. DHT yes, video no) without inventing many modes; mobile stays always-Client.

## N010 — Monetization: per-capability pricing primary; paid jobs secondary

**Date:** 2026-07-26  
**Decision:**

1. **Primary:** Billable capabilities — especially **message / audio / video relay** — each get a **pricing policy**: `volunteer` (free) or `paid` (rate + meter + **on-chain settlement**). Pricing is nested under the capability, not a third role and not a fake “settle” capability.
2. **Secondary:** **Accept paid jobs** is an optional later **marketplace** for discrete tasks. It complements relay pricing; it must **not** be treated as the only or primary way nodes charge others.
3. DHT / circuit relay may remain volunteer longer. Blockchain node provides settlement/identity rails; running a chain node is separate from advertising paid relay rates.
4. Clients may **pay** as consumers; they never host pricing.

**Rationale:** Product intent is “relays may charge other users and settle on chain.” A jobs board is useful but different (continuous infra vs one-off work). Agents must not collapse both into `accept_paid_jobs` alone.

## N011 — Separate `pp-node` binary for org / headless servers

**Date:** 2026-07-26  
**Decision:** Dedicated infrastructure (Brief org seeds, datacenter nodes, systemd/Docker daemons) runs **`pp-node`**, a headless binary that links the shared node runtime (libp2p host, bootstrap, later capabilities) **without** SDL/RmlUi. End-user apps remain **`pp-browser`** with in-process Client/Node (N009). Rejected as the production server path: `pp-browser --headless` / `--node-only` alone (GUI dependency weight, PIN/window coupling, poor ops images). Optional GUI `--node-only` may exist later for local dogfood only. Org seed listen stays **tcp/443**; in-app desktop Node preferred listen stays **18517** (N003). One core, two entrypoints — do not maintain a second networking stack.  
**Rationale:** Seed `3.208.41.58:443` and future org nodes need a small, non-interactive process; user desktops need a GUI. Same mesh protocols and PeerId model for both.

## N012 — Reachability status + guided network help

**Date:** 2026-07-26  
**Decision:** When Node participation matters, surface a **reachability status** (Reachable / Outbound only / Blocked / Unknown) based on measurable signals (private vs public listen IP, dial to seed, later dial-back / AutoNAT) — not definitive “you are behind a firewall” claims. Me → Network shows a Connection card; soft banner + guided sheet teach **port forwarding** (outbound-only + private IP), firewall allowlisting (blocked), and always offer **relay / skip**. Clients are not nagged to port-forward. `pp-node` gets ops status, not consumer copy. Cheap detection + help UI is phase **nr**; full AutoNAT / hole punch come later.  
**Rationale:** Mutual-help nodes fail silently behind NAT; users need to learn what is wrong and what they can do without feeling broken or blamed.

## N013 — Prefer IPv6 + UPnP/NAT-PMP before manual port forward

**Date:** 2026-07-26  
**Decision:** To become **Reachable**, try **IPv6 advertisement** and **UPnP / NAT-PMP / PCP** mapping before relying on the N012 manual port-forward checklist. UI: auto-try or one-tap “Open port on router”; on failure, fall back to guided manual forward. Org `pp-node` on public IPs need not use UPnP. Phase **nu**.  
**Rationale:** Most users never complete router configuration; IPv6 and UPnP convert far more Nodes to inbound-reachable.

## N014 — Contact-first relay preference (ask friends; serve friends)

**Date:** 2026-07-26  
**Updated:** 2026-07-30 (N020 — media hop algorithm; N014 remains intent / illustrative outcome)  
**Decision:** Relay hop selection (circuit / message / media) should **prefer contacts** (and household/trusted), then org seed, then (later) curated/public relays, with HTTP Brief as message fallback. When **hosting**, prefer capacity for **contacts/friends** before strangers (especially volunteer desktop Nodes). This is policy on capabilities — not a new role. Friends must opt in via Node + capability; never coerce. Phase **nf**, with or right after circuit-relay (**n3**).

**For `media_relay` (calls):** Do **not** implement N014 as a hardcoded stage list. Use the **risk-aware scorer + eligibility classes** in **N020** / calls **V023**. Short-term feasible set is **contacts ∪ org seed** only (no open public). The classic “friends then seed then public” ordering is the **expected volunteer outcome**, not the algorithm. Circuit (and later message) hops may keep a simpler N014-style preference until they grow similar abuse surface.

**Rationale:** Users want to ask people they know for routing and to help those people first; public paid relays are a backstop, not the default social path. Hardcoding stages fails under abuse, quality variance, and price gaming — see N020.

## N015 — Delivery order: reachability and circuit before DHT

**Date:** 2026-07-26  
**Decision:** Preferred ship order is **n1 → np (incl. dial-back) → nr → nu (IPv6/UPnP) → n3 (circuit-relay) → nf (contact-first) → n4 (billable relays + pricing) → directory/reputation → n2 (DHT) → chain/jobs/Home Node**. DHT remains in scope but is not the next feature after n1 by default.  
**Updated (N017, 2026-07-30):** Prefer **n4-media** (true A/V SFU) over packaging peer `message_relay` or paid UI; see N017.  
**Rationale:** Users feel “Node works” when they are reachable and can hop via trusted peers; Kademlia helps discovery later but does not unblock NAT or friend routing.

## N016 — Listen port busy: desktop fallback + persist; `pp-node` fail loud

**Date:** 2026-07-26  
**Decision:**

1. **Preferred desktop port** is **18517** (N003) in `listen_multiaddr` `/ip4/0.0.0.0/tcp/18517`.
2. **`pp-browser` (Node):** If bind on the preferred port fails, try a small consecutive range (e.g. **18517–18526**), then optionally an OS ephemeral port if the range is exhausted. **Persist** the successfully bound port into config (`listen_multiaddr`). Surface the **actual** listen port in Me → Network / Connection card and in any port-forward / UPnP UI — never coach “forward 18517” if the host bound another port.
3. **`pp-node` (ops):** By default **fail loudly** if the configured listen port cannot bind (especially **443**). Do not silently hop ports (firewall/systemd expectations). An explicit opt-in flag (e.g. `--listen-fallback`) may allow range fallback later; not the default.
4. Today’s code returns a generic listen failure and continues without libp2p — n1/np must replace that with N016 behavior + clear user/ops errors.

**Rationale:** Fixed preferred port keeps docs and UPnP simple; fallback avoids silent “Node on but dead” when 18517 is taken; ops seeds must not drift off 443 without the operator noticing.

---

## N017 — Split n4: media SFU first; message relay separate; pricing later

**Date:** 2026-07-30  
**Decision:** Split the former monolithic **n4** (message + audio + video + pricing) into tracks:

| Track | What ships | Blocks |
|-------|------------|--------|
| **n4-media** | Homegrown **blind** selective forwarder as **`media_relay`** (N018) on org **`pp-node`** (volunteer on) **and** desktop Node checkbox (**default on**). Call consumer in [p2p-av-calls](../p2p-av-calls/) (V020 / V021). | **a4** group calls |
| **message_relay** | Peer store-and-forward / inbox assist | **Not** an a4 gate. HTTP Brief relay remains the product offline path for now. Peer decentralization may never be the default. |
| **pricing** | N010 `volunteer \| paid` + settle | **Design / schema only** for n4-media. Ship volunteer SFU; paid UI/metering/chain **later** without redesigning caps. |

**Blind forwarder (not TURN, not full-mesh, not media-aware SFU):** Each participant uplinks once; relay fans out **opaque** packets by stream/publisher id. No call keys on relay; no codec decode. Rejected: full-mesh; TURN-only N−1 uploads; classic SFU that needs to see clear media.

**nf:** Circuit may keep N014-style preference. **Media hop pick** is **N020** / **V023** (no longer TBD). Message hops may keep using HTTP Brief without waiting on peer `message_relay`.

**N015 update:** Prefer **n4-media** next for calls unblocking over packaging message_relay or paid UI. Order sketch: … → n3 → (thin nf as needed) → **n4-media** → later message_relay / pricing UI / directory → n2 DHT.

**Rationale:** Offline message relay has different durability/trust/availability properties than realtime media; coupling them delayed calls. Billing must not gate volunteer seed SFU.  
**Alternatives:** Keep monolithic n4 (rejected); LAN full-mesh for a4 until SFU (rejected — V020); paid-first SFU (rejected).

---

## N018 — Blind `media_relay`; bandwidth budgets; volunteer default on

**Date:** 2026-07-30  
**Decision:** Implement n4-media as a **homegrown blind selective forwarder** (calls [V021](../p2p-av-calls/DECISIONS.md#v021--blind-media-forwarder-11-p2p-soft-migrate-to-group-sfu)):

| Rule | Detail |
|------|--------|
| **Blind** | Relay **does not** hold call media keys, **does not** decode Opus/H264, **does not** classify audio vs video by payload. Forward by publisher/subscriber / stream id + **byte-volume** limits only. |
| **Capability** | Prefer a single **`media_relay`** checkbox (supersedes shipping separate decode-aware `audio_relay` / `video_relay` pipelines). Legacy dual names in older sketches map to this one service + optional **capacity class / max_bps** advertisement. |
| **Bandwidth** | Node advertises a budget (e.g. max uplink aggregate bps or class). Clients decide whether Camera is allowed; relay rate-limits/drops by size if exceeded. |
| **Hosts** | Org **`pp-node`**: volunteer **`media_relay` on**. Desktop Node: checkbox **default on** (volunteer) when Node is enabled — user may turn off. Mobile Client: never hosts. |
| **Pricing** | Volunteer for ship; `pricing.*` schema stub only (N017). |
| **Pick / re-pick** | Call coordinator applies **N020** / **V023** (contacts ∪ seed short-term; re-pick on failure). |
**Rationale:** Matches product privacy (“relay must not know contents”); one module is enough for fan-out; friend Nodes need honest bandwidth limits without deep packet inspection of media.  
**Alternatives:** Classic media-aware SFU (rejected); separate audio/video services that inspect codecs (rejected); desktop default off until dogfood (superseded — default on volunteer).  
**Updates N017** host/checkbox wording from dual audio/video to blind `media_relay`.  
**Bandwidth detail:** **N019** / **V022**.  
**Pick detail:** **N020** / **V023**.  
**Framing / QoS:** **N021** / call mapping **V024**.

---

## N019 — `media_relay` ↑/↓ budgets, quotes, no surprise bills

**Date:** 2026-07-30  
**Decision:** `media_relay` capacity and future pricing use **separate upload and download** counters, session grants, and a **quote/accept** flow. Call-side rules: [V022](../p2p-av-calls/DECISIONS.md#v022--media-relay-bandwidth--quote-no-surprise-payer-bills). Hop pick algorithm: **N020** / **V023** (not a hardcoded N014 stage list).

### Advertisement / grant shape (names illustrative)

| Field | Meaning |
|-------|---------|
| `node_capacity_up` / `node_capacity_down` | Global Node ceilings (**C↑/C↓**) |
| `max_session_up` / `max_session_down` | Max this hop will grant a new call (**B↑/B↓** caps) |
| `default_per_user_up` / `default_per_user_down` | Default carve (**A↑/A↓**) |
| `pricing.media_relay` | `volunteer` \| `paid` + rate (N010); volunteer ships first |

### Session lifecycle

1. Coordinator asks hop for a **quote** given N + intent (voice vs video uplink class).  
2. Hop returns concrete **A↑/A↓**, **B↑/B↓**, estimate, rate, **billing ceiling**.  
3. **Session payer** (v1: call **initiator**) accepts → attach.  
4. Relay meters ↑ and ↓ **separately**; enforces by byte volume only (blind).  
5. Never charge above accepted ceiling; on pressure, rate-limit/drop / signal clients to disable Camera.  
6. Material change → **re-quote + re-accept**.

### Relation to pick policy

Quotes and ↑/↓ fit are **inputs** to the hop scorer in **N020** / calls **V023** (closed set short-term; pricing regulates later). Do not treat N014’s illustrative list as executable stage code for `media_relay`.

**Rationale:** Egress (↓) dominates SFU cost; payer protection must exist before paid mode; designing counters now avoids a volunteer→paid rewrite.  
**Alternatives:** Combined aggregate only (rejected); bill after the fact without ceiling (rejected); bake hardcoded priority tiers into this ADR (superseded by N020).

---

## N020 — Media hop pick: short-term closed set; pricing as regulation

**Date:** 2026-07-30  
**Decision:** `media_relay` hop selection is a **risk-aware scorer over eligibility classes**. **Revenue is not the goal**; the **pricing model regulates** scarcity, strangers, and abuse over time. Call twin: [V023](../p2p-av-calls/DECISIONS.md#v023--media-hop-pick-short-term-closed-set-pricing-regulates-later). Updates **N014** for media (intent retained; algorithm here).

### Thesis

Volunteer + **closed eligibility** first; quote/pricing schema always present (**rate 0**); paid/public classes unlock later to ration capacity and gate untrusted hops. **Abuse / flood / fraud** outrank friend preference and cheapness. Never pure `min(price)`.

### Short term (must-have with n4-media / calls a4)

| Rule | Detail |
|------|--------|
| **Feasible set** | **Contacts ∪ household/trusted ∪ org seed** only — **no open public** `media_relay` market |
| **Auth** | Attach only with authenticated call session (call_id + roster) |
| **Capacity + quote** | N019 / V022 ↑/↓ fit; quote + ceiling; initiator accept |
| **Pick** | Filter → score (**affinity + quality floor + residual capacity**; price = 0) → quote/accept |
| **Friends vs quality** | Affinity is a bonus; below quality floor → skip |
| **Re-pick** | Failure → cool-down exclude → same policy |
| **Provider** | Prefer contacts; limit/refuse strangers on volunteer desktop Nodes |

### Mid term

Curated public directory/allowlist; paid rates on public/overflow; free-for-contacts / paid-or-refuse strangers; stronger quality history; soft **concentration** penalty; re-quote when leaving volunteer ceiling.

### Long term

Bonds/stake; receipts/reputation; anti-dumping (outlier cheap = risk); stronger anti-capture; optional multi-homing; optional paid seed overflow for ops — not “sell SFU” as mission.

### Circuit / message

Circuit may keep simpler N014-style preference in **nf**. Peer `message_relay` remains deferred (N017); HTTP Brief stays message offline path.

**Rationale:** Closed set removes sybil/cheap-bait/capture for v1; scorer + rate-0 quotes avoid rewrite when regulation via price turns on.  
**Alternatives:** Hardcoded N014 stages for media (rejected); open public + min price in v1 (rejected); revenue-first paid SFU (rejected).

---

## N021 — Generic `media_relay` framing; QoS channel types

**Date:** 2026-07-30  
**Decision:** The `media_relay` hop is a **content-agnostic multiplexed datagram forwarder**. It must not assume audio/video or decode payloads. Clear **framing metadata** enables subscribe, byte accounting, and QoS. Call A/V **policy** (1:1 P2P and SFU) and role→type mapping: [V024](../p2p-av-calls/DECISIONS.md#v024--adaptive-call-media-11-p2p-and-sfu-generic-relay-channels). Future non-call apps may reuse the same hop.

### Packet / frame header (illustrative)

| Field | Meaning |
|-------|---------|
| **`stream_id`** | Publisher flow / uplink identity within the session |
| **`channel_id`** | Logical channel inside the stream |
| **`channel_type`** | **QoS policy class** (not a media codec name) |
| **`seq`** | Per-channel sequence (order / stale detection) |
| **`mark`** | Optional sync bit (“safe shed boundary” — e.g. video IDR). Not named `keyframe` in the relay API. |
| **payload** | Opaque bytes (E2E encrypted by the app) |

Optional later: priority, deadline/`ttl_ms`, explicit `payload_len` for metering.

### v1 `channel_type` policies (relay implements only these behaviors)

| Type | Behavior |
|------|----------|
| **`reliable_ordered`** | FIFO; deliver even if late; drop only under extreme backlog |
| **`latest_lossy`** | Prefer newest under congestion; may drop older; honor **`mark`** when shedding |
| **`best_effort`** | Forward if room; first class to shed when defined |

Relay **does not** interpret payload as Opus/H264/etc. Apps bind roles (audio, video_lo, …) to types at the edges.

### Session ops

- Receivers **subscribe** to `(stream_id, channel_id)` (or documented wildcards).  
- Fan-out only along subscribe edges.  
- Enforce **↑/↓** byte budgets (N019) on opaque sizes.  
- Under pressure, apply the channel_type policy; never invent media semantics.

### Reuse

Same capability may later carry other real-time opaque fan-out (e.g. in-call data, thumbnails) by picking a `channel_type` — no A/V-specific relay fork required. Optional future rename (e.g. `datagram_relay`) is cosmetic.

**Rationale:** Separates transport QoS from app codecs; enables stale-video drop without decrypt; keeps n4-media reusable.  
**Alternatives:** A/V-specific SFU API (rejected — couples hop to calls); fully opaque pipe with no seq/type (rejected — cannot implement latest-lossy safely); relay named `keyframe` as media concept (rejected — use generic **`mark`**).

---

## N022 — Libp2p investment; HTTP settle preferred; chain backup

**Date:** 2026-07-31  
**Decision:** Networking north star is **HTTP + libp2p** ([NETWORKING.md](../../docs/architecture/NETWORKING.md)). Continue **investing in the vendored libp2p fork** so it can serve peer discovery, dial/routing, transmission QoS, and price incentives under real network conditions. **HTTP backend** is preferred for org services and **pricing/settle UX** when reachable. **Direct blockchain settle** is a **backup** when HTTP is unavailable (or policy requires trust-minimized pay). Call media consumes this fabric ([V026](../p2p-av-calls/DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking)); WebRTC is not a mesh substitute.

| Track | Direction |
|-------|-----------|
| Reachability | Listen, UPnP, dial-back, strengthen circuit (toward PeerId-friendly paths); hole punch later as fork allows — program: [media-hop-reachability](../media-hop-reachability/) (**in-stack**, not app gather) |
| Discovery | Contacts ∪ bootstrap now; directory; DHT per N015 timing |
| Transmission | N021 framing/QoS; lossy audio-friendly paths; budgets N019 |
| Incentives | Quotes/ceilings; volunteer → paid as regulation (N020); contact-first admission |
| Settle | HTTP ledger/receipts preferred; chain settle ADR/detail when backup path ships |

**Rationale:** One peer stack to deepen; HTTP optimizes backend and payment UX; chain avoids hard-fail when Brief HTTP is blocked.  
**Alternatives:** WebRTC for peer media forever (rejected — V026); chain-first payments for every hop (rejected — UX/ops); HTTP-only peers (rejected — mesh product).

---

## N023 — Relay scope and domain bridging (not geography tiers)

**Date:** 2026-08-01  
**Decision:** Model relay routing with **connectivity domains** and **relay scope tags** (`link` → `site` → `social` → `org` → `public`), not user-selected “local / country / global” tiers.

| Rule | Detail |
|------|--------|
| **Problem framing** | Nested **partitions** (LAN, egress/firewall, global). Relay value = useful **boundary crossings**. |
| **Consumer** | **Escalate scope** narrow→wide: filter → score (affinity + **bridge score** + capacity + price + reputation) → quote (N019) → attach → re-pick. Prefer narrowest working scope. |
| **Provider** | **Auto-cap scope** from reachability + capability + pricing — no tier picker. Volunteer default: `link \| site \| social`; `public` requires paid + opt-in. |
| **Partition escape** | When seed/global fails, use **bridge score** within social graph — not geo IP. |
| **Roles** | Gateway / island store / infrastructure are **topology outcomes**, not user job titles. |
| **Ownership** | Policy in **p2p-mesh** ([RELAY_SCOPE.md](RELAY_SCOPE.md)); dialability in [media-hop-reachability](../media-hop-reachability/) (H001). |
| **Messages** | HTTP Brief remains org/global durability fallback; peer `message_relay` may add link/site/island queues later. |
| **Short term** | Consumer mask stops at **`org`**; `public` ineligible. `link`/`site` order eligible **contacts** only — no LAN strangers. |
| **Algorithm** | Outer scope bands + inner N020 scorer — not the rejected hardcoded N014 stage list. |

**Rationale:** Local relays stay high-value for small groups without forcing users to configure tiers; global relays scale audience for reachable ops nodes; minimal UX via inference from existing `ReachabilitySignals` and contact graph. Extends N014/N020 without replacing closed-set short-term policy.  
**Alternatives:** Rename or expand media-hop-reachability to own scope (rejected — blurs H001 stack vs policy); new top-level project for routing only (rejected — p2p-mesh already owns hop policy); explicit geo/country tier picker (rejected — privacy + wrong abstraction).  
**Spec:** [RELAY_SCOPE.md](RELAY_SCOPE.md). **Phase:** [ns](PHASES.md#ns--relay-scope-and-domain-bridging-n023).
