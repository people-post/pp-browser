# Current state — ADP / AMP

**As of:** 2026-08-30

## Landed (L1)

- Project docs + ADRs A001–A020
- `src/base/adp/` → `pp_base_adp` (Asio-free)
- Wire v1 + HMAC-SHA256-128, best-effort + reliable, path migrate, OsUdp
- `pp_browser_adp_test` (26 tests)
- Contract: [`docs/contracts/ADP.md`](../../docs/contracts/ADP.md)

## Landed (stack spec — D0)

- [STACK.md](STACK.md) — four-layer AMP model
- [`docs/contracts/AMP-SESSION.md`](../../docs/contracts/AMP-SESSION.md) — L2 MSH + full AEAD
- [`docs/contracts/AMP-CHANNEL.md`](../../docs/contracts/AMP-CHANNEL.md) — L3 mux, ch0, fragmentation
- [PHASES.md](PHASES.md) — D1–D9 migration checklist

## Transitional (legacy TCP path)

- **Opus side-path:** `CallMediaAdpPath` + TCP-hello `adp_*` + `CallMediaAdpDogfood.h` (A008–A011)
- **Production mesh:** still TCP + Noise + Yamux via `Libp2pHost`
- Delete transitional Opus path when **D6** ships

## Next (implementation)

1. **D1** — `src/base/mesh/session/` + MSH + `pp_browser_amp_session_test`
2. **D2** — `src/base/mesh/channel/` + `ChannelSession` + ch0
3. **D3** — L3 fragmentation
4. **D4** — `PeerLinkManager` + `MeshPump`

See [PHASES.md](PHASES.md) for full ordering.
