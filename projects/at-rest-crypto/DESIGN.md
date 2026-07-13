# Design — at-rest encryption

**Normative:** [docs/contracts/AT_REST_ENCRYPTION.md](../../docs/contracts/AT_REST_ENCRYPTION.md).

## Principles

1. **PIN wraps a random DEK** — never derive the long-term file key from the PIN alone.
2. **Secrets-first** — identity private keys and PSK material; leave `thread.db` plaintext (chat-storage D048) until a later phase.
3. **Same AEAD as wire crypto** — XChaCha20-Poly1305 via libsodium; Argon2id for PIN KDF.
4. **Atomic whole-file writes** — tmp in same directory, fsync, rename.
5. **No backward compatibility** — wipe profile data when formats change; forgotten PIN = wipe profile.

## Module map

```
app/Bootstrap
        │
        ├─ ProfileSecretsService (base/crypto)  ← vault, Unlock, DEK fan-out
        └─ MessagingHub (feature/messaging)     ← EnsureMessagingReady after unlock

base/crypto
  PinDefaults · PinKeyDeriver · DataKeyVault · FileCipher · PinResolver
  ProfileSecretsService · IDekConsumer
        │
        ├─ people/IdentityStore  → identity.enc
        └─ SqlitePskSessionStore → chat_targets PSK columns
base/data
  AtomicFileWrite  → all JSON/blob replaces
  UserPreferences  → pin_is_default in preferences.json
feature/ui
  PinGateController  → chooser / create / unlock overlay
  SecuritySettingsSection  → Me → Security status + Change PIN
```

**Unlock split:** `ProfileSecretsService::Unlock(pin)` creates/unlocks `vault.bin` and fans out DEK to registered `IDekConsumer`s. `MessagingHub::EnsureMessagingReady()` loads identity and starts libp2p/P2P (messaging-only). `PinGateController` calls both for E2E/register flows.

## Threat model (v1)

| Adversary | Protected | Not protected |
|-----------|-----------|---------------|
| Offline disk theft (locked) | Identity + PSK ciphertext | Transcripts in `thread.db` |
| Wrong PIN | AEAD/KDF fail closed | Brute force limited only by Argon2 cost |
| Crash mid-write | Prior file intact (atomic rename) | — |
| Memory while unlocked | — | DEK/plaintext in process RAM |

## PIN policy

- Collected in-app (`PinGateController` overlay). CLI/env optional for automation.
- **No vault:** defer until first secrets use; three-way chooser (A007) — custom PIN, Just continue (app default), or cancel.
- **Vault + `pin_is_default`:** silent unlock at bootstrap and UI load; toast nudges user to Me → Security.
- **Vault + custom PIN:** blocking unlock after UI load (mandatory; no cancel).
- **Change PIN:** Me → Security when unlocked; clears `pin_is_default`.
- Default PIN (`123456`) is weak by design — document clearly; not a substitute for user-chosen PIN.
- No recovery key in v1.
