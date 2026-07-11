# At-rest encryption

Normative spec for profile secrets on disk. Planning ADRs: [projects/at-rest-crypto/](../projects/at-rest-crypto/).

**Related:** [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md) (wire E2E), [CONFIGURATION.md](CONFIGURATION.md).

## Overview

- **PIN per profile** — collected in-app; `--pin` / `PP_BROWSER_PIN` optional for tests/CI.
- Random **32-byte DEK** wrapped by a PIN-derived KEK (Argon2id).
- **XChaCha20-Poly1305** encrypts identity and PSK material under the DEK.
- Whole-file replaces use **atomic write** (tmp → fsync → rename).
- **Forgotten PIN:** wipe the profile. No recovery key in v1.
- **No migrators** from plaintext layouts — delete data dir when upgrading during development.

## Algorithms

| Layer | Algorithm | Library |
|-------|-----------|---------|
| PIN → KEK | Argon2id (`crypto_pwhash`) | libsodium |
| Wrap DEK / file AEAD | XChaCha20-Poly1305 | libsodium |
| DEK / KEK size | 32 bytes | — |

Interactive `crypto_pwhash` ops/mem limits are stored in `vault.bin` so unlock matches creation.

## On-disk files

```
profiles/{id}/
  vault.bin       # wrapped DEK + KDF params
  identity.enc    # AEAD(identity JSON) under DEK
  contacts.json   # plaintext (atomic write)
  threads/
    profile.db    # PSK columns ciphertext; other columns plaintext
    {thread_id}/thread.db   # plaintext transcripts (D048)
```

### `vault.bin` (v1)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Magic `PPBV` |
| 4 | 1 | Version `1` |
| 5 | 8 | `opslimit` (u64 LE) |
| 13 | 8 | `memlimit` (u64 LE) |
| 21 | 16 | Salt |
| 37 | 24 | Nonce |
| 61 | var | Ciphertext + MAC of DEK |

AAD for wrap: `vault-dek|{profile_id}|1`.

### Payload AEAD

Uses the same blob layout as message payloads (`EncryptedPayload`: version + nonce + ciphertext).  
AAD examples: `identity|{profile_id}|1`, `psk|{profile_id}|1`.

### Identity plaintext (inside `identity.enc`)

JSON fields: `public_key_b64`, `private_key_b64`, `kem_*`, `nickname`, `relay_user_id`, `registered`.  
(`peer_id` remains in-memory only.)

## Unlock flow

1. App starts without requiring a PIN (local AI/chat works).
2. **If `vault.bin` exists:** blocking unlock modal after UI load.
3. **If no vault:** PIN create is deferred until first secrets use (Register, Secure message, PSK actions, etc.) via `EnsureSecretsUnlocked`. User may cancel (“Not now”) and retry later.
4. Optional automation: `--pin` or `PP_BROWSER_PIN` still unlocks at bootstrap for tests/CI.
5. Lock on exit clears DEK (`sodium_memzero`).

## Threat model (v1)

| Capability | Protected by |
|------------|--------------|
| Offline read of identity / PSK without PIN | AEAD + Argon2id |
| Crash mid JSON write | Atomic rename |
| Transcript theft from `thread.db` | **Not** protected (D048) |

## Test fixtures

Unit/integration tests may call `IdentityStore::SetDek` / `SqlitePskSessionStore::SetDek` with a fixed DEK, or pass `--pin` / `PP_BROWSER_PIN`.
