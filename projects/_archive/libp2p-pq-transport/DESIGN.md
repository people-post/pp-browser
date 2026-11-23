# Full-PQ libp2p — design

**Related ADRs:** [DECISIONS.md](DECISIONS.md). **Phases:** [PHASES.md](PHASES.md).

## Threat model

| Adversary | Protected by |
|-----------|--------------|
| Harvest-now-decrypt-later on Noise session keys | ML-KEM-768 ephemeral encaps in handshake |
| Forge PeerId / Noise identity | ML-DSA-65 identity key + signature over `noise-libp2p-static-key:` ‖ static KEM pk |
| Classical break of X25519 / Ed25519 on mesh | Removed from product path (hard cut) |

App E2E (chat/call AEAD, account ML-DSA) is unchanged and remains the content confidentiality layer.

## Stack (product)

```
TCP
 └─ /noise-mlkem768/1.0.0
     suite: Noise_XXkem_MLKEM768_ChaChaPoly_SHA256
     identity: ML-DSA-65 (KeyType wire = 4)
 └─ Yamux
 └─ app protocols
```

## Noise XXkem (KEM tokens)

Pattern shape matches classical XX message order:

```
-> E
<- E, DHEE, S, DHES
-> S, DHSE
```

**KEM semantics** (replace symmetric DH):

- `E` / `S`: send ML-KEM-768 **public** key (1184 B); `S` is encrypt-and-hash once a chaining key exists.
- `DHEE` / `DHES` / `DHSE` / `DHSS` on **write**: `Encaps(remote_pk)` → append **ciphertext** (1088 B) → `MixKey(shared_secret)`.
- Same tokens on **read**: consume ciphertext → `Decaps(local_sk, ct)` → `MixKey(shared_secret)`.

Which remote public key is used matches classical Noise XX (writer encapsulates to the public half of the classical `dh(local_priv, remote_pub)` pair; reader decapsulates with the matching local private key).

Static Noise keypair is an ephemeral-per-handshake ML-KEM keypair (same role as classical Noise static). Identity payload signs:

```
"noise-libp2p-static-key:" || static_mlkem_public_key
```

with the device **ML-DSA-65** private key.

## Sizes

| Item | Bytes |
|------|-------|
| ML-KEM-768 pk | 1184 |
| ML-KEM-768 sk | 2400 |
| ML-KEM-768 ct | 1088 |
| ML-KEM shared secret | 32 |
| ML-DSA-65 pk | 1952 |
| ML-DSA-65 sk | 4032 |
| ML-DSA-65 sig | 3309 |
| Noise frame max | 65535 |

## Wire KeyType

Provisional `KeyTypeWire::kMlDsa65 = 4` until multiformats assigns a permanent code. PeerId = multihash of marshalled `PublicKey { type=4, data=ml-dsa-pk }` (sha2-256 / `Qm…` for large keys; not identity multihash `12D3KooW…`).

## Libraries

Fork providers under `src/lib/libp2p/src/crypto/` link vendored `mldsa_native` / `mlkem_native`. App `base/crypto` wrappers stay for account/auto-key; libp2p must not depend on `base/`.

## Test vectors (KATs)

Frozen in unit tests as phases land:

1. ML-DSA keygen/sign/verify + PeerId from marshalled pk (deterministic seed where API allows; otherwise round-trip + size asserts).
2. Single handshake transcript: MixHash / MixKey order for XXkem with fixed ephemeral seeds if injectable; otherwise two-peer interoperability test.
3. Identity payload sign bytes: prefix + 1184-byte static pk → ML-DSA verify.
