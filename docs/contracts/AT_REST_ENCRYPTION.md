# At-rest encryption

**Tier:** contract

Normative spec for profile secrets on disk. Planning ADRs: [projects/at-rest-crypto/](../../projects/at-rest-crypto/).

**Related:** [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md) (wire E2E), [DATA_LAYOUT.md](DATA_LAYOUT.md), [COMPATIBILITY.md](COMPATIBILITY.md).

## Overview

- **PIN per profile** — collected in-app; `--pin` / `PP_BROWSER_PIN` optional for tests/CI.
- **First secrets use:** identity fork — **I'm new on this device** (then the PIN chooser) or **I already have an account** (PIN for this device, then paste a link payload). `--pin` / CI still take the create path.
- **`pin_is_default`** in `preferences.json` — when true, bootstrap and UI load silently unlock with the default PIN; cleared after **Change PIN** in Me → Security.
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
  preferences.json  # profile prefs incl. pin_is_default (schema v4)
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

JSON fields: `schema_version` (**2**), device `public_key_b64` / `private_key_b64` (Ed25519), account ML-DSA + `account_id`, **account** `kem_*` (ML-KEM-768 — **M015**), `nickname`, `relay_user_id`, `brief_llm_api_key`, `registered`, `registration_expires_at`, optional `initiation_floor` (pp_credit minor units; missing → 0).  
Unversioned blobs migrate on unlock and are rewritten with `schema_version`. Unsupported **newer** versions fail load.  
(`peer_id` remains in-memory only.)  
AEAD AAD (`identity|{profile_id}|1`) is independent of the plaintext `schema_version` field.

## Unlock flow

1. App starts without requiring a PIN (local AI/chat works).
2. **If `vault.bin` exists and `pin_is_default` is true:** silent unlock at bootstrap and after UI load (no modal). If silent unlock fails, fall back to the blocking unlock modal.
3. **If `vault.bin` exists and custom PIN:** blocking unlock modal after UI load (no cancel).
4. **If no vault:** defer until first secrets use (Register, Secure message, PSK actions, etc.) via `ProfileUnlockGate::EnsureUnlocked`. Show an **identity fork**, then:
   - **I'm new on this device** — the **three-way PIN chooser:**
     - **Set a PIN** — create flow with confirm.
     - **Just continue** — create vault with `kDefaultProfilePin` (`123456`), set `pin_is_default`, proceed; toast points to Me → Security.
     - **Not now** — cancel; retry on next secrets action.
   - **I already have an account** — choose a PIN for *this* device (Set a PIN / Just continue / Not now), then paste a `pp-browser-link-device-v1` payload. Wrap the **shared DEK** (`CreateWithDek`); do not mint a new Brief person first. Me → Security only **copies** a payload (export). If this profile is already a person, **Reset this profile** in Storage, then the same fork.
5. Optional automation: `--pin` or `PP_BROWSER_PIN` unlocks at bootstrap for tests/CI (takes precedence over silent default unlock).
6. **Change PIN:** Me → Security when unlocked (`DataKeyVault::ChangePin`); clears `pin_is_default`.
7. Lock on exit clears DEK (`sodium_memzero`) in the vault and all registered `IDekConsumer`s.

Default PIN is intentionally weak — offline disk theft with `pin_is_default` true is trivial. Users who want real protection should set or change their PIN in Me → Security.

### DEK consumers

[`ProfileSecretsService`](../../src/base/crypto/ProfileSecretsService.h) owns the vault and fans out the unlocked DEK to registered [`IDekConsumer`](../../src/base/crypto/IDekConsumer.h) stores (`SetDek` / `ClearDek`). Today: `IdentityStore`, `SqlitePskSessionStore` (registered from `MessagingHub::Initialize`). To add a new encrypted store:

1. Implement `IDekConsumer`; encrypt with `FileCipher` and a unique AAD purpose (`purpose|profile_id|schema`).
2. Register via `ProfileSecretsService::RegisterDekConsumer` during init (typically from the feature that owns the store).
3. Gate first use with `ProfileUnlockGate::EnsureUnlocked` (profile unlock + messaging-ready port when messaging is needed), or check `ProfileSecretsService::IsUnlocked()`.
4. Document the on-disk path and AAD purpose here.

**Messaging:** E2E/P2P actions also require `MessagingHub::IsMessagingReady()` after profile unlock. Application fills `ProfileUnlockPorts::ensure_messaging_ready` from the hub; [`PinGateController`](../../src/feature/ui/PinGateController.h) is presentation only (identity fork / chooser / unlock / link-paste overlay).

Unit/integration tests may still call `SetDek` directly with a fixed DEK (no vault required).

## Threat model (v1)

| Capability | Protected by |
|------------|--------------|
| Offline read of identity / PSK without PIN | AEAD + Argon2id |
| Offline read when `pin_is_default` | **Weak** — known default PIN in source |
| Crash mid JSON write | Atomic rename |
| Transcript theft from `thread.db` | **Not** protected (D048) |

## Test fixtures

Unit/integration tests may call `IdentityStore::SetDek` / `SqlitePskSessionStore::SetDek` with a fixed DEK, or pass `--pin` / `PP_BROWSER_PIN`. New `IDekConsumer` stores should accept the same direct `SetDek` injection in tests.
