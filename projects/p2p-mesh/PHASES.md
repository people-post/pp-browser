# P2P mesh — phases

**[DESIGN.md](DESIGN.md) is the authoritative specification** (concept-first, present tense). **This file orders work only** — checklists, delivery sequence, and traceability. Rationale: [DECISIONS.md](DECISIONS.md). Implementation truth: [CURRENT_STATE.md](CURRENT_STATE.md).

Preferred order is **N015** as amended by **N017**: n1 → np → nr → nu → n3 → nf (thin) → **n4-media** → later message_relay / pricing UI → … → n2 (DHT later).

**Pre-blockchain name directory (N029):** after n-dir/n2 landings, follow **[PRE_CHAIN_PLAN.md](PRE_CHAIN_PLAN.md)** packages **nd1 → nd5** before on-chain names (Phase D).

## n0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md` + `AGENTS.md`
- [x] N008–N021 (through hop pick, ↑/↓ quotes, **generic framing / QoS channel types**)
- [x] **N022** — libp2p investment; HTTP settle preferred; chain backup
- [x] **N023** — relay scope / domain bridging spec ([RELAY_SCOPE.md](RELAY_SCOPE.md))
- [x] Renamed project folder `libp2p-node-roles` → **`p2p-mesh`**
- [x] **N029** — name directory north star ([NAME_DIRECTORY_NORTH_STAR.md](NAME_DIRECTORY_NORTH_STAR.md)); pre-chain plan ([PRE_CHAIN_PLAN.md](PRE_CHAIN_PLAN.md))

## n1 — Role shell + bootstrap + Network UI

- [x] `MeshConfig`: `bootstrap_peers` (seed tcp/443), `node_enabled`, preferred listen `/ip4/0.0.0.0/tcp/18517`
- [x] Desktop busy-port fallback **18517–18526** (+ optional ephemeral); persist actual port (N016)
- [x] Clear error / UX if Node listen ultimately fails (no silent “no libp2p”)
- [x] `ConfigJson` + `config.json.example`
- [x] `ResolveMeshRole` (mobile → Client; desktop × `node_enabled`)
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
- [x] Light UI: “Friends first” (default on) if needed
- [x] Message path may keep **HTTP Brief** without peer `message_relay`
- [x] Align docs/UI copy with **N020** for media (closed set; not hardcoded stages)

## n4-media-sm — `media_relay` attach SM (N026) — docs before code

Pairs with calls [V033](../p2p-av-calls/DECISIONS.md#v033--transport-session-machines-not-host-wide-inbound-sm) / [SESSION_MACHINES.md](../p2p-av-calls/SESSION_MACHINES.md). Spec: [MEDIA_RELAY_ATTACH.md](MEDIA_RELAY_ATTACH.md). Prefer after call-media SM lands.

- [x] N026 + MEDIA_RELAY_ATTACH design doc
- [x] Freeze with calls V033 s1; call-media s2a landed first
- [x] s3a — Replace inbound `while (!session)` with per-stream phase + `media_relay_attach phase=` logs
- [x] s3b — Client `AcceptAndAttach` phase machine; Detach aborts waiter; no late stream install after timeout
- [x] Circuit compose loopbacks (PreferLocal reattach + circuit quote/attach fan-out)

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

## n-dir — Mesh directory consumers (before n2)

**Work plan:** [DISCOVERY_ROADMAP.md](DISCOVERY_ROADMAP.md#track-n-dir--wire-mesh-directory-into-consumers). API + pp-node publish landed (N027); **consumer wiring landed**.

- [x] `MeshDirectoryCache` + periodic `ListMeshNodes` refresh
- [x] `CollectDirectoryHopCandidates` + `MeshHopAffinity::DirectoryNode`
- [x] Wire circuit/media hop paths + `RegisterPeerDirectEndpoint`
- [x] Bridge score prefers directory when seed unreachable (`seed_dial_ok` → skip seeds)
- [x] Phase E smoke + docs (manual) — lab Amp/DHT + Brief `/mesh/nodes` probe done; live www `mesh_node` publish still open

## n2 — DHT (later per N015)

**Work plan:** [DISCOVERY_ROADMAP.md](DISCOVERY_ROADMAP.md). AMP-native Kademlia; not pp-ledger BitTorrent DHT.

- [x] **n2-spec:** ADR N028 + `docs/contracts/MESH_DHT.md` + config schema stub
- [x] **n2-core:** FIND_PEER when `Node && capabilities.dht` (default off)
- [x] **n2-caps:** Signed capability records in DHT
- [x] **n2-hard:** Rate limits / reputation (trail v1)
- [ ] Never on Client/mobile
- [x] Do not start n2-core until **n-dir** acceptance passes

## nd — Pre-chain name directory (N029 Phases A–C)

**Work plan:** [PRE_CHAIN_PLAN.md](PRE_CHAIN_PLAN.md). Do **before** on-chain names (Phase D). n-dir/n2 consumer tracks are largely done; this hardens the phone-book seam.

- [x] **nd1** — `INameDirectory` / `NameRecord` seam (load-bearing for chain swap)
- [x] **nd2** — Record field fidelity + resolve-by-account on `INameDirectory` (manual Phase E smoke still open)
- [x] **nd3** — `directory.providers[]` + HTTP failover
- [x] **nd4** — Amp directory twin (`MESH_DIRECTORY_AMP.md` / `/pp-mesh/directory/1.0.0`)
- [x] **nd5** — `ledger_gateway` capability vocab + hop collector (dial path / UI deferred)
- [x] First-release bar: **nd1 + nd2** (nd3 preferred); nd4/nd5 landed in same track
- [x] Optional DHT lab smoke — `scripts/pp_node_dht_smoke.sh`

## Later horizons

- [ ] Capability directory / curated public (N020 mid)
- [ ] Soft reputation / receipts; bonds; anti-dumping / anti-capture (N020 long)
- [ ] Schedules & resource caps; Home Node pack
- [ ] Gradual HTTP → peer message_relay dual-run (only if product wants it)

## ns — Relay scope & domain bridging (N023)

Docs-first; implement after n4-media stable. See [RELAY_SCOPE.md](RELAY_SCOPE.md).

- [x] ADR **N023** + RELAY_SCOPE design doc
- [x] `RelayScope` enum + scope mask; `RankMediaHopsEscalating`; provider serve mask (ns1)
- [x] Provider: reachability-aware stranger limit in `ApplyMeshAdmissionPolicies`
- [ ] Consumer: wire escalate ranker in circuit path; capability ads on Identify (call-control `caps` on invite/accept — V030; Identify still open)
- [ ] Bridge score when `seed_dial_ok == false` or target undialable direct (incl. multi-hop reach signals — [H008](../media-hop-reachability/DECISIONS.md#h008--multi-hop-circuit-chains-planned))
- [ ] Optional Me → Network scope preset (auto default; contacts / wider)
- [ ] Island / Bluetooth store-and-forward sketch (message_relay track; no hard dependency)

## ns3 — Multi-hop circuit policy

Pairs with stack [L3.5](../media-hop-reachability/PHASES.md#l35--multi-hop-circuit-v2). Spec: [MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md). ADR: [N024](DECISIONS.md#n024--immediate-relay-as-service-broker).

- [x] ADR: R1 as service broker (A pays R1; R1 subcontracts R2 + B; bundled media + SLA)
- [ ] Config: `circuit_relay.max_hops` (default 3; no hardcoded protocol max)
- [ ] R1 upstream relay scorer (margin-aware; scope + N014 preference)
- [ ] R1 retail quote to A (bundled circuit + media + latency tier); R1↔B wholesale quote
- [ ] Consumer bridge score: `r1_reaches_target` incl. subcontract hints (Identify / probe cache)
- [ ] Inter-relay settlement (R1 pays R2, B — HTTP preferred N022)
- [ ] SoftMigrate brokered attach mode vs direct attach ([H005](../media-hop-reachability/DECISIONS.md#h005--circuit-last-resort-bill-media-hop)); broker quote scoped to **call-agreed B**
- [x] Re-pick bounds: R1 = path only; B′ = coordinator SoftMigrate (V023)
- [x] Quote renewal: auto-extend path-only (same B); re-accept when B′ or rate/ceiling changes
- [ ] Per-hop admission unchanged; loop detection

## nm — Mobile call-scoped listen (N025)

**In progress.** Spec: [N025](DECISIONS.md#n025--mobile-call-scoped-listen-on-wi-fi-not-full-node). Call consumer: [V027](../p2p-av-calls/DECISIONS.md#v027--mobile-call-scoped-listen-on-wi-fi).

- [x] ADR + DESIGN (N025 / V027)
- [x] Wi‑Fi + foreground-call gating; start/stop ephemeral `host->listen`
- [x] Publish advertised LAN addrs via Identify during eligible session
- [x] Optional in-call `media_relay` (N≥3) — contacts-only admission; off on cellular
- [x] Integrate with call bring-up / teardown (CallSessionManager ↔ NodeRuntime)
- [ ] Later: opt-in **Help on Wi‑Fi** toggle (mode 3); no full Node UI on mobile
- [x] Docs: [PLATFORMS.md](../../docs/architecture/PLATFORMS.md), hop L4 consume notes

## ns2 — LAN mDNS (contacts-only)

Pairs with hop L4 PeerId-only reachability. Spec: [RELAY_SCOPE.md](RELAY_SCOPE.md) (`link` scope).

- [x] `_pp-browser._tcp` mDNS announce when Node or mobile ephemeral listen active
- [x] Browse → upsert `PeerAddressBook` / endpoints for **known contact PeerIds only** (N020 closed set)
- [x] Wire `ConversationsHub::TickMesh` + contact list refresh
- [ ] Bridge score uses mDNS / same-subnet signals (consumer circuit path)
