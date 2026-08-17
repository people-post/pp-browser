# Full-PQ libp2p transport

**Status:** Phases 0–5 landed (manual device smoke optional)  
**Owner:** Hongwei + agents  
**Stable refs (promote as phases ship):** [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md), [LIBP2P_STREAMS.md](../../docs/architecture/LIBP2P_STREAMS.md), [AT_REST_ENCRYPTION.md](../../docs/contracts/AT_REST_ENCRYPTION.md)  
**Related:** [e2e-message-crypto](../e2e-message-crypto/) (account ML-DSA / ML-KEM), [multi-device-account](../multi-device-account/) (device vs account keys)

## One-line goal

Hard-cut the live mesh path (**TCP → Noise → Yamux**) to **full post-quantum bar (3)**: device PeerId / Noise identity auth on **ML-DSA-65**, Noise secrecy on **ML-KEM-768 only** (no X25519, no classical `/noise` fallback).

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Threat model, suite, wire sizes, KATs |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |
| [PHASES.md](PHASES.md) | Rollout checklist |
| [DECISIONS.md](DECISIONS.md) | ADRs P001–P006 |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| 0 | Project + ADRs | **Done** |
| 1 | Fork ML-DSA identity crypto | **Done** |
| 2 | Fork ML-KEM-only Noise | **Done** |
| 3 | App device-identity hard cut | **Done** |
| 4 | Docs / ADR amendments | **Done** |
| 5 | Dogfood gate | **Done** (manual device smoke optional) |

## Locked product decisions

- Noise: **ML-KEM-768 only** (not hybrid XXhfs)
- Compatibility: **hard cut** (wipe/regenerate device keys)
- Device ML-DSA-65 is **separate** from account ML-DSA-65
