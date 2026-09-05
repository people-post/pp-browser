# Phases — libp2p-pq-transport

## Phase 0 — Project + ADRs

- [x] README / DESIGN / DECISIONS / PHASES / CURRENT_STATE
- [x] ADRs P001–P006

## Phase 1 — Fork ML-DSA identity crypto

- [x] `Key::Type::MlDsa65` + `KeyTypeWire::kMlDsa65 = 4`
- [x] `mldsa_provider` + `CryptoProviderImpl` / marshaller / validator
- [x] Unit tests: keygen, marshal, sign/verify, PeerId

## Phase 2 — Fork ML-KEM-only Noise

- [x] ML-KEM DiffieHellman/Kem provider (`dhName` MLKEM768)
- [x] Handshake DH tokens carry encaps ciphertext
- [x] Protocol id `/noise-mlkem768/1.0.0`
- [x] Two-peer handshake test + buffer sizing for large keys

## Phase 3 — App hard cut

- [x] IdentityStore device ML-DSA; schema bump / wipe legacy
- [x] PeerIdUtil / Libp2pHost / ConversationsHub bind path
- [x] Link-device keeps local device key

## Phase 4 — Docs

- [x] Amend M003/M008/E025 + LIBP2P_* / AT_REST / DATA_LAYOUT
- [x] projects/README entry

## Phase 5 — Dogfood gate

- [x] Purge product 32-byte device-key assumptions (Libp2pHost / ConversationsHub / NodeBootstrap / PeerIdUtil / IdentityStore)
- [x] Unit + Yamux/Noise stream tests; `pp-browser` link
- [x] Wipe notes in COMPATIBILITY + CURRENT_STATE (manual desktop/Android smoke left to operators)