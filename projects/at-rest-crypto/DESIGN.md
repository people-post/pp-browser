# Design — at-rest encryption

**Normative:** [docs/AT_REST_ENCRYPTION.md](../../docs/AT_REST_ENCRYPTION.md).

## Principles

1. **PIN wraps a random DEK** — never derive the long-term file key from the PIN alone.
2. **Secrets-first** — identity private keys and PSK material; leave `thread.db` plaintext (chat-storage D048) until a later phase.
3. **Same AEAD as wire crypto** — XChaCha20-Poly1305 via libsodium; Argon2id for PIN KDF.
4. **Atomic whole-file writes** — tmp in same directory, fsync, rename.
5. **No backward compatibility** — wipe profile data when formats change; forgotten PIN = wipe profile.

## Module map

```
feature/messaging (MessagingHub unlock)
        │
        ▼
base/crypto
  PinKeyDeriver · DataKeyVault · FileCipher · PinResolver
        │
        ├─ people/IdentityStore  → identity.enc
        └─ SqlitePskSessionStore → chat_targets PSK columns
base/data
  AtomicFileWrite  → all JSON/blob replaces
```

## Threat model (v1)

| Adversary | Protected | Not protected |
|-----------|-----------|---------------|
| Offline disk theft (locked) | Identity + PSK ciphertext | Transcripts in `thread.db` |
| Wrong PIN | AEAD/KDF fail closed | Brute force limited only by Argon2 cost |
| Crash mid-write | Prior file intact (atomic rename) | — |
| Memory while unlocked | — | DEK/plaintext in process RAM |

## PIN policy

- Mandatory per profile.
- Provide via `--pin` or `PP_BROWSER_PIN`.
- First run creates `vault.bin`; subsequent runs unlock.
- No recovery key in v1.
