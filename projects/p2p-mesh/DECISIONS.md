# P2P mesh — decisions

## N001 — Desktop node default / mobile default client

**Date:** 2026-07-26  
**Updated:** 2026-08-01 (mobile **default** stays Client; scoped listen — **N025**)  
**Decision:** Exactly two **roles**: **client** and **node**. Desktop effective role is **node** when `node_enabled` is true (default). Mobile **`node_enabled` is ignored**; effective role is **Client by default** (no always-on listen, no Me → Network Node UI).  
**Exception (planned):** **Call-scoped / Wi‑Fi-scoped listen** on mobile — **N025** — not a third role; ephemeral listen while eligible, not full Node.  
**Rationale:** Desktops can host infrastructure; mobiles default outbound-only for battery and OS limits. LAN direct dial and in-call hops need a narrow listen path without turning every phone into an always-on mesh node.

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
| **Hosts** | Org **`pp-node`**: volunteer **`media_relay` on**. Desktop Node: checkbox **default on** (volunteer) when Node is enabled — user may turn off. Mobile: **default Client, no host**; optional in-call / Wi‑Fi-scoped relay per **N025**. |
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

---

## N024 — Immediate relay as service broker

**Date:** 2026-08-01  
**Updated:** 2026-08-01 (bundled media + SLA; supersedes circuit-only wording)  
**Status:** Accepted (plan — pairs with [H008](../media-hop-reachability/DECISIONS.md#h008--multi-hop-circuit-chains-planned))  
**Decision:** When consumer **A** uses immediate relay **R1** (circuit path and/or brokered call attach), **A pays R1 only**. R1 is the **single commercial and SLA face** — priced and experienced like “the hop,” even when R1 subcontracted upstream **`circuit_relay`** (R2+) and **`media_relay`** (B) capacity behind the scenes.

| Rule | Detail |
|------|--------|
| **Consumer payer** | **A pays R1 only** — one quote + billing ceiling with R1 |
| **Consumer UX** | A does not quote or pay R2 or B directly on the brokered path; R1’s rate may differ from B’s wholesale rate |
| **Upstream circuit** | R1 chooses R2+; R1 pays upstream circuit cost; must keep **positive margin** when paid |
| **Downstream media (brokered calls)** | R1 subcontracts **`media_relay`** on **call-agreed B**; **R1 absorbs B’s wholesale** in its retail quote to A and may add markup for path + **latency / delivery guarantee** |
| **SLA owner** | R1 owns **path + attach delivery** to the **call-agreed** target; see **re-pick bounds** below |
| **Direct attach (unchanged)** | When A **direct-dials B** without a broker, **A pays B** per N019 / [H005](../media-hop-reachability/DECISIONS.md#h005--circuit-last-resort-bill-media-hop) — friend volunteer SFU, no R1 markup |
| **Volunteer R1** | May bundle volunteer upstream (R2, B) at rate 0 to A; still one relationship with R1 |

### Re-pick bounds (calls — B is roster-bound)

**B** is the group’s agreed blind SFU for a **`call_id`** (coordinator pick + authenticated attach per V023/N020). R1 does **not** unilaterally replace B.

| Failure | Who acts | What may change |
|---------|----------|-----------------|
| Upstream circuit (R2+, path) | **R1** | Alternate route — **same B** |
| R1↔B attach / path to B | **R1** (retry) | **Same B** |
| **B** unavailable | **Call coordinator** (SoftMigrate) | **B′** — call-level; roster + `call_id` auth |

### Quote renewal (brokered attach)

| Situation | Consumer (A) action |
|-----------|---------------------|
| **Path-only retry** to **same B** (R2′, same tier T, same retail rate/ceiling) | **Auto-extend** — no re-accept; original R1 quote remains in force |
| Coordinator picks **B′** (new SFU PeerId) | **Re-accept** with R1 when retail **rate or billing ceiling** changes; new quote scoped to B′ |
| B′ but **unchanged** retail rate/ceiling and tier T | May **auto-extend** if R1 and policy confirm equivalent wholesale — product may still show lightweight ack |
| A can **direct-dial B′** | **Direct attach** to B′ (H005); skip broker |

Paid mode: never exceed the **accepted ceiling** without explicit re-accept (same rule as N019/V022).

**Rationale:** One payer and one SLA owner matches “R1 feels like the real B at a different price”; enables latency guarantees only the orchestrator can offer on **path to** the agreed SFU; R1 optimizes subcontract mix for margin. **B** remains call-scoped — SFU re-pick is coordinator policy (V023), not broker discretion.  
**Alternatives:** A pays B separately while using R1 for circuit only (rejected — splits SLA, confuses UX); pay-each-hop (rejected); R1 forbidden from marking up B (rejected — no incentive to broker); **R1 unilateral B swap** (rejected — breaks roster / group-agreed media hub).  
**Implementation notes (later):** R1↔B wholesale quote/attach protocol; inter-relay settlement (HTTP preferred per N022); SoftMigrate **brokered** vs **direct** attach modes.  
**Spec:** [MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md). **Stack phase:** [L3.5](../media-hop-reachability/PHASES.md#l35--multi-hop-circuit-v2). **Policy phase:** [ns3](PHASES.md#ns3--multi-hop-circuit-policy).

---

## N025 — Mobile call-scoped listen on Wi‑Fi (not full Node)

**Date:** 2026-08-01  
**Status:** Accepted (**implemented** — gating in `MessagingHub` / `MobileEphemeralListenGate`; LAN QA pending)  
**Decision:** Mobile stays **Client by default** (N001). Add **narrow, gated listen** so phones can be **dialed by PeerId on LAN** and serve as **in-call hops** without always-on Node behavior.

### Participation modes (mobile)

| Mode | Listen | Host `media_relay` | When |
|------|--------|-------------------|------|
| **Client (default)** | No | No | Always — current behavior |
| **Call participant** | Ephemeral | Optional — **contacts / in-call only**, capped | Foreground **active call** while on **Wi‑Fi**; publish listen addrs via Identify for direct libp2p / in-call hop pick |
| **Wi‑Fi helper (opt-in, later)** | While enabled | Volunteer, **social scope only**, strict ↑/↓ caps | User toggle **Help on Wi‑Fi**; **off on cellular**; no Me → Network capability matrix |

Desktop **Node** (`node_enabled`) is unchanged. Mobile does **not** gain a master **Help the network** toggle in v1 of this feature.

### Rules

1. **No always-on mobile Node** — listen starts/stops with eligibility (call foreground + Wi‑Fi, or explicit Wi‑Fi helper).  
2. **Cellular** — do not listen or host `media_relay` for strangers by default; seed / desktop / circuit paths remain primary off-LAN (V008).  
3. **Background / killed app** — **does not** rely on idle listen. Incoming ring stays **`call_wake` → fetch → UI → outbound dial** (V006). Ephemeral listen supplements **active-session** reachability only.  
4. **Scope** — mobile `media_relay` (when enabled) uses **`social` / in-call admission only** — no `public`, no paid overflow on phone (N023 provider caps).  
5. **Battery / data** — prefer **listen without relay** for 1:1 callee; relay only when N≥3 or explicit in-call hop policy needs it.  
6. **Implementation** — same stack as desktop (Identify, address book, `IsPeerDialable`); gating in app/mesh policy, not a second libp2p integration.

### Call impact

See [V027](../p2p-av-calls/DECISIONS.md#v027--mobile-call-scoped-listen-on-wi-fi). Unblocks LAN **PeerId-only** direct paths when combined with discovery (mDNS or seed-mediated Identify) and hop **L4**.

**Rationale:** “Mobile never listens” blocked LAN 1:1 libp2p and in-call mobile hops; full Node on mobile is wrong for battery and OS background limits. Scoped listen gets most LAN benefit with bounded cost.  
**Alternatives:** Full mobile Node (rejected); forever Client-only (rejected for LAN PeerId goal); app `call_hop_addrs` (rejected — H007); listen on cellular always (rejected).  
**Supersedes:** Absolute “never listen” wording in **N018** / **H006** — default remains no listen.  
**Phase:** [nm](PHASES.md#nm--mobile-call-scoped-listen-n025).

---

## N026 — `media_relay` per-stream attach state machine

**Date:** 2026-08-07  
**Status:** Accepted (**s3a+s3b done** — inbound + client attach phases in code; SoftMigrate / PreferLocal dogfood gate next; after call-media [V033](../p2p-av-calls/DECISIONS.md#v033--transport-session-machines-not-host-wide-inbound-sm) s2a)  
**Decision:** Formalize the inbound `media_relay` control handshake (`quote` → `accept` → `attach`) as a **per-inbound-stream** flat enum + `Apply(event)` state machine. Keep **`HostSession`** as a session object (participants, meters, fan-out) — do **not** model the entire QoS/fan-out graph as phases. Client outbound attach should gain a matching small event vocabulary so it does not remain on raw `settled` promises after the host path is cleaned up.

| Rule | Detail |
|------|--------|
| **Spec** | [MEDIA_RELAY_ATTACH.md](MEDIA_RELAY_ATTACH.md) |
| **Style** | Same as calls V033 / `CallLifecycle` — no framework, INFO phase/event logs |
| **Guards** | Encode [HOST_RECEIVE_POLICY](../p2p-av-calls/HOST_RECEIVE_POLICY.md) / V032 admit rows (contact/scope, max 4 sessions, max 8 participants, auth stub) |
| **Non-goals** | Host-wide inbound SM; changing N021 framing; putting hop eligibility/pricing into the machine |
| **Order** | Prefer call-media SM first (threading/waiter lessons), then this |

**Rationale:** The `while (!session)` control loop is an implicit SM with attach/reattach races. Making phases explicit hardens SoftMigrate and PreferLocal without rewriting fan-out.

**Alternatives rejected:** One giant HostSession hierarchical SM; SM per JSON op instance (lose multi-message stream context); bundling into CallLifecycle.

**Cross-link:** N018–N021; V032 / V033; [SESSION_MACHINES.md](../p2p-av-calls/SESSION_MACHINES.md).
