# P2P mesh — phases

Preferred order is **N015** as amended by **N017**: n1 → np → nr → nu → n3 → nf (thin) → **n4-media** → later message_relay / pricing UI → … → n2 (DHT later).

## n0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md` + `AGENTS.md`
- [x] N008–N019 (infra, caps, pricing, `pp-node`, reachability, UPnP/IPv6, contact-first, delivery order, listen **18517** + busy-port, **n4-media split**, **blind media_relay**, **↑/↓ quotes**)
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

## nf — Contact-first relay preference (N014)

- [ ] Consumer priority for **circuit** (and later SFU): contacts → household/trusted → org seed → public → HTTP fallback where applicable
- [ ] Provider priority: prefer serving contacts when hosting relay (esp. volunteer Node)
- [ ] Wire into circuit hop selection first; **SFU pick ranking TBD** (design discussion — do not invent final order yet)
- [ ] No coercion — friend must have capability on
- [ ] Light UI: “Prefer contacts for routing” (default on) if needed
- [ ] Message path may keep **HTTP Brief** without peer `message_relay`

## n4-media — Blind media forwarder (N017 / N018; unblocks a4)

- [ ] Homegrown **blind** selective forwarder — no media keys, no codec decode, no A/V payload classification
- [ ] Single **`media_relay`** capability; advertise **C↑/C↓**, grant **B↑/B↓**, carve **A↑/A↓** (N019)
- [ ] Quote / accept + billing ceiling before attach (volunteer rate 0 OK); never bill above ceiling
- [ ] Org `pp-node`: volunteer **`media_relay` on** for [p2p-av-calls](../p2p-av-calls/) (V008 / V020 / V021 / V022)
- [ ] Desktop Node: Me → Network checkbox **default on** (volunteer); user may disable
- [ ] Call consumer: group path + soft-migrate from 1:1 P2P (V021)
- [ ] `pricing.*` schema stub — volunteer only in this phase
- [ ] SFU **pick priority / scorer TBD** — do not hardcode final rank yet

## n4-message / pricing — deferred (N017)

- [ ] Peer `message_relay` (store-and-forward) — separate from media; HTTP Brief remains default offline path
- [ ] Paid UI / metering / on-chain settle (N010) when volunteer capacity is insufficient
- [ ] Blockchain rails / accept_paid_jobs later (secondary)

## n2 — DHT (later per N015)

- [ ] Kademlia when `Node && capabilities.dht`
- [ ] Never on Client/mobile
- [ ] Do not jump here immediately after n1

## Later horizons

- [ ] Capability directory (still contact-first)
- [ ] Soft reputation / receipts
- [ ] Schedules & resource caps; Home Node pack
- [ ] Gradual HTTP → peer message_relay dual-run (only if product wants it)
