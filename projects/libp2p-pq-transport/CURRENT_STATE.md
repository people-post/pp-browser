# Current state — libp2p-pq-transport

**As of:** 2026-08-17

## Landed in tree

- Phase 0 ADRs P001–P006
- Phase 1: `Key::Type::MlDsa65`, fork `mldsa_provider`, CryptoProvider / marshaller / validator, tests
- Phase 2: Noise XXkem ML-KEM-768, protocol `/noise-mlkem768/1.0.0`, handshake state KEM tokens, `noise_xxkem_test`
- Phase 3: `identity.enc` schema **3**, device ML-DSA keys, `PeerIdFromMlDsaPublicKey`, `Libp2pHost` / ConversationsHub / NodeBootstrap bind path; legacy Ed25519 fail-closed
- Phase 4: M003/M008/E025 + AT_REST / DATA_LAYOUT / LIBP2P_* / COMPATIBILITY docs amended
- Phase 5: Product 32-byte device-key bind paths removed; unit + Yamux/Noise stream tests green; `pp-browser` links

## Verified in CI-style local run

- `mldsa_provider_test` — pass
- `noise_xxkem_test` — pass
- `PeerIdUtil*` / `IdentityStore*` — pass
- `StreamsRegression.*Noise*` (full host TCP+Noise+Yamux) — pass
- `pp-browser` executable builds

## Dogfood (manual)

Wipe local profiles (pre-v3 `identity.enc`) via `scripts/wipe_local_profile.sh` before running. Desktop: register → dial peer → chat/call. Confirm protocol id `/noise-mlkem768/1.0.0` and PeerIds are sha2-256 style (`Qm…`) from ML-DSA pubkeys.

## Note

Classical Ed25519 remains available inside the fork for TLS/examples/tests; the **product** host path does not advertise `/noise` or bind Ed25519 device keys.
