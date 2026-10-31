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
- **Forgotten PIN:** wipe the profile; the account (not its messages) comes back from its recovery phrase (§ Account recovery).
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
    profile.db    # PSK columns + preview_enc ciphertext; other columns plaintext (user_version 4)
    {thread_id}/thread.db   # content_enc + value_enc ciphertext; metadata plaintext (user_version 2 — D102)
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

JSON fields: `schema_version` (**3**), device `public_key_b64` / `private_key_b64` (**ML-DSA-65** — PeerId / Noise), account ML-DSA + `account_id`, **account** `kem_*` (ML-KEM-768 — **M015**), `nickname`, `relay_user_id`, `brief_llm_api_key`, `registered`, `registration_expires_at`, optional `initiation_floor` (pp_credit minor units; missing → 0), device `device_kem_*` (M021), optional `recovery_seed_b64` — the 32-byte recovery seed the account ML-DSA and KEM keys derive from (HKDF-SHA256, salt `pp-browser-recovery-v1`, labels `account-ml-dsa` / `account-ml-kem`; [M023](../../projects/multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) §2). Every account minted since M023 s1 has one; one minted before (random keys) or a pp-node (its own seed) has none. Its phrase is the BIP-0039 English encoding of the seed (24 words), never BIP-0039's PBKDF2 seed. A link bundle carries it when present (`recovery_seed_b64`), refused unless it derives the bundle's account keys. Optional `card_intent` (`fresh` / `join`, M021 d4): how the device enters its account's device list on its next card — set by a restore (fresh) or a link (join), cleared once done ([ACCOUNT_CARD § Main device](ACCOUNT_CARD.md)).
Pre-v3 (Ed25519 device keys) **fail closed** — wipe profile and recreate ([libp2p-pq-transport P004](../../projects/libp2p-pq-transport/DECISIONS.md#p004--hard-cut--wipe-amend-m003m008e025)). Unsupported **newer** versions fail load.
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
   - **I already have an account** — choose a PIN for *this* device (Set a PIN / Just continue / Not now), then paste a `pp-browser-link-device-v1` payload. Wrap the **shared DEK** (`CreateWithDek`); do not mint a new Brief person first. Me → Security only **copies** a payload (export). The payload (`LinkDeviceCodec`, valid 15 minutes) carries the account ML-DSA and KEM key pairs, the DEK, the relay user id, the nickname, the issuing device's account card (required on export: the new device signs itself into it — [ACCOUNT_CARD.md](ACCOUNT_CARD.md)) and the recovery seed when the account has one. It carries **no sessions**: each device opens its own per device pair ([M021](../../projects/multi-device-account/DECISIONS.md#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits)); an older build's `public_psks` key is ignored. If this profile is already a person, **Reset this profile** in Storage, then the same fork.
5. Optional automation: `--pin` or `PP_BROWSER_PIN` unlocks at bootstrap for tests/CI (takes precedence over silent default unlock).
6. **Change PIN:** Me → Security when unlocked (`DataKeyVault::ChangePin`); clears `pin_is_default`.
7. Lock on exit clears DEK (`sodium_memzero`) in the vault and all registered `IDekConsumer`s.

Default PIN is intentionally weak — offline disk theft with `pin_is_default` true is trivial. Users who want real protection should set or change their PIN in Me → Security.

### DEK consumers

[`ProfileSecretsEngine`](../../src/foundation/crypto/ProfileSecretsEngine.h) owns the vault and fans out the unlocked DEK to registered [`IDekConsumer`](../../src/foundation/crypto/IDekConsumer.h) stores (`SetDek` / `ClearDek`). Today: `IdentityStore`, `SqlitePskSessionStore`, `SqliteThreadStore`, `CallMediaKeyStore`, `AttachmentFetchWorkflow`, `Libp2pChatBlobService` (registered from `ConversationsHub`). To add a new encrypted store:

1. Implement `IDekConsumer`; encrypt with `FileCipher` and a unique AAD purpose (`purpose|profile_id|schema`).
2. Register via `ProfileSecretsEngine::RegisterDekConsumer` during init (typically from the feature that owns the store).
3. Gate first use with `ProfileUnlockGate::EnsureUnlocked` (profile unlock + messaging-ready port when messaging is needed), or check `ProfileSecretsEngine::IsUnlocked()`.
4. Document the on-disk path and AAD purpose here.

**Messaging:** E2E/P2P actions also require `ConversationsHub::IsMessagingReady()` after profile unlock. Application fills `ProfileUnlockPorts::ensure_messaging_ready` from the hub; [`PinGateController`](../../src/gui/PinGateController.h) is presentation only (identity fork / chooser / unlock / link-paste overlay).

Unit/integration tests may still call `SetDek` directly with a fixed DEK (no vault required).

## Threat model (v1)

| Capability | Protected by |
|------------|--------------|
| Offline read of identity / PSK without PIN | AEAD + Argon2id |
| Offline read when `pin_is_default` | **Weak** — known default PIN in source |
| Crash mid JSON write | Atomic rename |
| Transcript theft from `thread.db` | AEAD under DEK when vault exists (D102); metadata plaintext |

### Transcript AEAD (D102)

| Purpose | AAD |
|---------|-----|
| Message body pack | `transcript-body\|{profile_id}\|{thread_id}\|{message_id}\|1` |
| Sidebar preview | `transcript-preview\|{profile_id}\|{thread_id}\|1` |
| Thread memory value | `transcript-memory\|{profile_id}\|{thread_id}\|{key}\|1` |

Plaintext inside message AEAD: `TranscriptBodyCodec` v1 JSON envelope (`chat_payload_b64`, `text`, `payload`, optional `content_rml`, `chat_actions`, optional `working_set_json`, optional `ai_image_json` = reference `{content_hash, mime, width, height}` to the private-CAS image of an AI question). Helpers: `TranscriptCipher`, `TranscriptBodyCodec` in `src/base/messaging/`.

### Attachment blob AEAD (a5 / C007 / C011)

| Purpose | AAD |
|---------|-----|
| Private CAS block | `cas-private\|{profile_id}\|{content_id_hex}\|1` (`BuildCasPrivateAad`) |

Durable attachment bytes live under `{profile}/cas/private/blocks/{aa}/{bb}/{content_id_hex}` (PPBA + `FileCipher` under profile DEK). Thread `blobs/` is not used. Session plaintext for RmlUi `<img>` / OS open may be materialized under `blobs_view/` while unlocked and is wiped on `ClearDek` (`WipeAllAttachmentViewCaches`), which also clears `AttachmentPlaintextMemoryCache` (C011). A private video whose declared size is above `AttachmentLimits::max_receive_bytes` (`AttachmentAllowsInlinePrivateView`) is not downloaded at all and so has no `blobs_view` copy; CAS ingest is unchanged. Helpers: `SaveAttachmentPlaintext` / `LoadAttachmentPlaintext` / `EnsureAttachmentViewPath` in `AttachmentCache`. Images the user sends to the AI assistant are not stored durably: their thumbnails are session plaintext under the reserved pseudo thread `threads/ai-images/blobs_view/` and are wiped by the same `WipeAllAttachmentViewCaches` path.


## Test fixtures

Unit/integration tests may call `IdentityStore::SetDek` / `SqlitePskSessionStore::SetDek` / `SqliteThreadStore::SetDek` with a fixed DEK, or pass `--pin` / `PP_BROWSER_PIN`. New `IDekConsumer` stores should accept the same direct `SetDek` injection in tests.


Unlock + `EnsureMessagingReady` exposes local messaging without waiting on Amp mesh; see [SERVICE_CAPABILITIES.md](../architecture/SERVICE_CAPABILITIES.md).

## Account recovery (recovery phrase, account locker)

An account minted since M023 s1 derives its account keys from a 32-byte recovery seed kept in `identity.enc` (§ Identity plaintext); the seed also derives the keys of the **account locker**, the encrypted snapshot of the account's state a provider holds for it ([ACCOUNT_LOCKER.md](ACCOUNT_LOCKER.md)). Me → Security shows the recovery phrase (the seed as 24 words) only after the profile PIN is entered. A fresh install restores from the typed phrase under a PIN the user chooses; the DEK is new (nothing sealed under the old one comes back — the locker holds the state). Whoever has the phrase has the account.

The earlier PIN-sealed local account backup (iOS Keychain) is retired ([A011](../../projects/at-rest-crypto/DECISIONS.md#a011--local-account-backup-and-restore-unsupported-in-the-product)); an item an earlier build wrote is deleted once messaging is up, until app version 1.1.0 ([DATA_LAYOUT](DATA_LAYOUT.md#retired-local-account-backup-ios-keychain-outside-the-data-dir)).

