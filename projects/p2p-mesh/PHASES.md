# P2P mesh — phases

Preferred order is **N015**: n1 → np → nr → nu → n3 → nf → n4 → … → n2 (DHT later).

## n0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md` + `AGENTS.md`
- [x] N008–N016 (infra, caps, pricing, `pp-node`, reachability, UPnP/IPv6, contact-first, delivery order, listen **18517** + busy-port)
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

- [ ] Extract shared **node runtime** (no second libp2p stack)
- [ ] Executable **`pp-node`** without SDL/RmlUi
- [ ] Non-interactive unlock; config; signal wait; ops listen `:443` example
- [ ] **Fail loud** if configured listen port busy (N016) — no silent fallback by default
- [ ] **Dial-back probe** API for clients’ reachability tests (feeds **nr**)
- [ ] systemd/Docker sketch; not `pp-browser --headless` as prod path

## nr — Reachability status + guided help (N012)

- [ ] Status: Reachable / Outbound only / Blocked / Unknown
- [ ] Signals: private vs public IP; dial seed; inbound-seen; **dial-back** via seed
- [ ] Me → Network Connection card; soft banner; guided sheets; Skip/relay
- [ ] i18n; `pp-node --status` for ops

## nu — IPv6 + UPnP/NAT-PMP (N013)

- [ ] Advertise usable global IPv6 when present; reflect in Connection card
- [ ] UPnP / NAT-PMP / PCP try or one-tap; re-test reachability
- [ ] On failure → existing N012 manual port-forward sheet
- [ ] Skip UPnP requirement for public `pp-node` seeds

## n3 — Circuit-relay capability

- [ ] Fork/protocol work (absent today); `capabilities.circuit_relay` + checkbox
- [ ] Org `pp-node` seed may offer circuit-relay
- [ ] HTTP Brief relay remains fallback
- [ ] Pricing optional (often volunteer initially)

## nf — Contact-first relay preference (N014)

- [ ] Consumer priority: contacts → household/trusted → org seed → public → HTTP fallback
- [ ] Provider priority: prefer serving contacts when hosting relay (esp. volunteer Node)
- [ ] Wire into circuit (then message/media) hop selection
- [ ] No coercion — friend must have capability on
- [ ] Light UI: “Prefer contacts for routing” (default on) if needed

## n4 — Billable relays + pricing (N010)

- [ ] Message / audio / video relay + `pricing.*` (volunteer \| paid)
- [ ] Still honor N014 when picking hops
- [ ] Org `pp-node` seeds: volunteer **audio/video SFU** on for [p2p-av-calls](../p2p-av-calls/) mobile path (V008); pricing may stay volunteer initially
- [ ] Blockchain rails / accept_paid_jobs later (secondary)

## n2 — DHT (later per N015)

- [ ] Kademlia when `Node && capabilities.dht`
- [ ] Never on Client/mobile
- [ ] Do not jump here immediately after n1

## Later horizons

- [ ] Capability directory (still contact-first)
- [ ] Soft reputation / receipts
- [ ] Schedules & resource caps; Home Node pack
- [ ] Gradual HTTP → peer message_relay dual-run
