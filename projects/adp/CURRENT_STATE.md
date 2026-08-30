# Current state — ADP

**As of:** 2026-08-30

## Landed

- Project docs + ADRs A001–A011
- `src/base/adp/` → `pp_base_adp` (Asio-free)
- Wire v1 + HMAC-SHA256-128, best-effort + reliable, path migrate, OsUdp
- `pp_browser_adp_test` (26 tests)
- Contract: [`docs/contracts/ADP.md`](../../docs/contracts/ADP.md)
- **Slice 1 (Opus dogfood):** `CallMediaAdpPath` + HKDF `K_assoc`, call-media hello `adp_*` fields, `CallLibp2pMediaBridge` Opus→ADP when `libp2p.adp_opus=true` (default **off**), TCP fallback (A011)

## Dogfood checklist (LAN)

1. Both peers: set `"libp2p": { "adp_opus": true }` in profile `config.json`.
2. 1:1 call on same LAN (or one side public); confirm audio.
3. Set `adp_opus` false (or omit) → Opus stays on TCP call-media only.

## Next

- Hole punch / dual-NAT
- libp2p / MeshHost generic ADP transport
- Video fragmentation over ADP
- pp-ledger transport over ADP
