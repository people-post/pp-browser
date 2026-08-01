# P2P mesh — phases

Preferred order is **N015** as amended by **N017**: n1 → np → nr → nu → n3 → nf (thin) → **n4-media** → later message_relay / pricing UI → … → n2 (DHT later).

## n0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md` + `AGENTS.md`
- [x] N008–N021 (through hop pick, ↑/↓ quotes, **generic framing / QoS channel types**)
- [x] **N022** — libp2p investment; HTTP settle preferred; chain backup
- [x] Renamed project folder `libp2p-node-roles` → **`p2p-mesh`**

## n1 — Role shell + bootstrap + Network UI

- [x] `Libp2pConfig`: `bootstrap_peers` (seed tcp/443), `node_enabled`, preferred listen `/ip4/0.0.0.0/tcp/18517`
- [x] Desktop busy-port fallback **18517–18526** (+ optional ephemeral); persist actual port (N016)
- [x] Clear error / UX if Node listen ultimately fails (no silent “no libp2p”)
- [x] `ConfigJson` + `config.json.example`
- [x] `ResolveLibp2pRole` (mobile → Client; desktop × `node_enabled`)
- [x] Skip `host->listen` for Client; register bootstrap peers
- [x] `SessionConfigFromApp` via `Platform::IsMobile()`
- [x] Me → Network master toggle + i18n (no caps / pricing / reachability UI yet); surface **actual** listen port when Node
- [x] Docs + unit tests; refresh CURRENT_STATE / README

## np — Headless `pp-node` + dial-back (N011)

- [x] Extract shared **node runtime** (no second libp2p stack)
- [x] Executable **`pp-node`** without SDL/RmlUi
- [x] Non-interactive unlock; config; signal wait; ops listen `:443` example
- [x] **Fail loud** if configured listen port busy (N016) — no silent fallback by default
- [x] **Dial-back probe** API for clients’ reachability tests (feeds **nr**)
- [x] systemd/Docker sketch; not `pp-browser --headless` as prod path

## nr — Reachability status + guided help (N012)

- [x] Status: Reachable / Outbound only / Blocked / Unknown
- [x] Signals: private vs public IP; dial seed; inbound-seen; **dial-back** via seed
- [x] Me → Network Connection card; soft banner; guided sheets; Skip/relay
- [x] i18n; `pp-node --status` for ops

## nu — IPv6 + UPnP/NAT-PMP (N013)

- [x] Advertise usable global IPv6 when present; reflect in Connection card
- [x] UPnP / NAT-PMP / PCP try or one-tap; re-test reachability
- [x] On failure → existing N012 manual port-forward sheet
- [x] Skip UPnP requirement for public `pp-node` seeds

## n3 — Circuit-relay capability

- [x] Fork/protocol work (absent today); `capabilities.circuit_relay` + checkbox
- [x] Org `pp-node` seed may offer circuit-relay
- [x] HTTP Brief relay remains fallback
- [x] Pricing optional (often volunteer initially)

## nf — Contact-first preference (N014) + media scorer prep (N020)

- [x] Circuit: simpler contacts → seed preference (N014 intent)
- [x] Provider: prefer serving contacts when hosting (esp. volunteer Node)
- [x] No coercion — friend must have capability on
- [x] Light UI: “Prefer contacts for routing” (default on) if needed
- [x] Message path may keep **HTTP Brief** without peer `message_relay`
- [x] Align docs/UI copy with **N020** for media (closed set; not hardcoded stages)

## n4-media — Blind media forwarder (N017–N021; unblocks a4)

- [x] Homegrown **content-agnostic** forwarder — no media keys, no codec decode
- [x] Framing: **`stream_id \| channel_id \| channel_type \| seq \| mark`** + opaque payload (N021)
- [x] QoS types v1: **`reliable_ordered`**, **`latest_lossy`** (+ optional `best_effort`); subscribe by `(stream_id, channel_id)`
- [x] Single **`media_relay`** capability; **C↑/C↓**, **B↑/B↓**, **A↑/A↓** (N019)
- [x] Quote / accept + billing ceiling; volunteer rate 0
- [x] Hop pick: **contacts ∪ org seed** only; filter → score; re-pick (N020 / V023)
- [x] Auth before attach; provider prefer contacts / limit strangers
- [x] Org `pp-node`: volunteer **`media_relay` on**; desktop checkbox **default on**
- [x] Call consumer maps audio/video_lo/video_hi per **V024** (same policy as 1:1 P2P backend) — **a4 thin** (single video layer)
- [x] `pricing.*` schema stub — pricing regulates later (not revenue-first)

## n4-message / pricing UI — deferred (N017 / N020 mid)

- [ ] Peer `message_relay` — separate; HTTP Brief remains
- [ ] Curated public + paid rationing UI when needed (N020 mid)
- [ ] Bonds / reputation / anti-capture (N020 long)
- [ ] Blockchain rails / accept_paid_jobs later (secondary)

## n2 — DHT (later per N015)

- [ ] Kademlia when `Node && capabilities.dht`
- [ ] Never on Client/mobile
- [ ] Do not jump here immediately after n1

## Later horizons

- [ ] **ns** — Relay scope + domain bridging ([RELAY_SCOPE.md](RELAY_SCOPE.md), N023)
- [ ] Capability directory / curated public (N020 mid)
- [ ] Soft reputation / receipts; bonds; anti-dumping / anti-capture (N020 long)
- [ ] Schedules & resource caps; Home Node pack
- [ ] Gradual HTTP → peer message_relay dual-run (only if product wants it)

## ns — Relay scope & domain bridging (N023)

Docs-first; implement after n4-media stable. See [RELAY_SCOPE.md](RELAY_SCOPE.md).

- [x] ADR **N023** + RELAY_SCOPE design doc
- [ ] `RelayScope` enum + scope mask on hop candidates / capability ads
- [ ] Consumer: escalate scope in ranker (extend `RankMediaHops` / circuit order)
- [ ] Provider: scope cap from `ReachabilitySignals` + pricing (generalize admission policies)
- [ ] Bridge score when `seed_dial_ok == false` or target undialable direct
- [ ] Optional Me → Network scope preset (auto default; contacts / wider)
- [ ] Island / Bluetooth store-and-forward sketch (message_relay track; no hard dependency)
