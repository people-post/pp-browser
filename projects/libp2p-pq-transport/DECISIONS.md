# Decisions — libp2p-pq-transport

## P001 — Full PQ threat bar (transport secrecy + PeerId/auth)

**Date:** 2026-08-17  
**Decision:** Release mesh crypto meets bar (3): Noise session secrecy and device PeerId/Noise identity authentication are post-quantum. App E2E layers already PQ or PQ-adequate.  
**Rationale:** Pre-release hard cut; avoid shipping classical Noise/PeerId when account identity is already ML-DSA/ML-KEM.  
**Alternatives:** Bar (2) HFS-only with classical Ed25519 PeerId; wait for upstream cpp-libp2p.

## P002 — ML-KEM-768-only Noise; protocol id `/noise-mlkem768/1.0.0`

**Date:** 2026-08-17  
**Decision:** Suite `Noise_XXkem_MLKEM768_ChaChaPoly_SHA256`. Multistream id `/noise-mlkem768/1.0.0` only on the product host. No X25519 hybrid; no classical `/noise` advertise. XX message order with KEM encaps/decaps on DH tokens (see DESIGN).  
**Rationale:** Matches aggressive E026/M008 ML-KEM-only posture; closed fleet allows hard cut.  
**Alternatives:** Hybrid XXhfs (industry draft); dual negotiate with `/noise` fallback.

## P003 — Wire KeyType MlDsa65 = 4 (provisional)

**Date:** 2026-08-17  
**Decision:** Extend `Key::Type` / `KeyTypeWire` with `MlDsa65 = 4`. Document in LIBP2P_UPSTREAM until multiformats assigns a permanent code.  
**Rationale:** Need a distinct marshalled pubkey for PeerId; 0–3 are classical.  
**Alternatives:** Private high enum; wait for upstream assignment.

## P004 — Hard cut + wipe; amend M003/M008/E025

**Date:** 2026-08-17  
**Decision:** No classical device Ed25519 or `/noise` on product path. Legacy `identity.enc` device Ed25519 fails closed / wipe. Amend multi-device M003/M008 and e2e E025: device endpoint key is ML-DSA-65.  
**Rationale:** Pre-release; mixed fleets are not a goal.  
**Alternatives:** Soft dual-stack during dogfood.

## P005 — Vendored natives inside the fork

**Date:** 2026-08-17  
**Decision:** `mldsa_native` / `mlkem_native` linked from libp2p crypto/Noise providers. No `base/crypto` dependency from `lib/libp2p`.  
**Rationale:** Layer rule `base → lib`; avoid upward edges.  
**Alternatives:** Inject providers from app; duplicate algorithm code.

## P006 — Identity payload still binds static Noise key

**Date:** 2026-08-17  
**Decision:** Keep prefix `noise-libp2p-static-key:`; static material is the handshake ML-KEM-768 public key (1184 B). Signed with device ML-DSA-65.  
**Rationale:** Preserves libp2p Noise identity binding semantics under KEM static keys.  
**Alternatives:** Sign only identity pubkey; drop static binding.
