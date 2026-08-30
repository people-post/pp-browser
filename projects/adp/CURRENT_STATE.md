# Current state — ADP

**As of:** 2026-08-30

## Landed

- Project docs + ADRs A001–A007
- `src/base/adp/` → `pp_base_adp` (Asio-free)
- Wire v1 + HMAC-SHA256-128, best-effort + reliable, path migrate, OsUdp
- `pp_browser_adp_test` (26 tests)
- Contract: [`docs/contracts/ADP.md`](../../docs/contracts/ADP.md)

## Next (later plans)

- libp2p / MeshHost bridge
- Hole punch / relay
- pp-ledger transport over ADP
