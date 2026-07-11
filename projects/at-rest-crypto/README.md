# At-rest crypto

**Status:** Implementation in progress (atomic writes + vault + identity/PSK encryption)  
**Owner:** Hongwei + agents  
**Stable refs:** [docs/AT_REST_ENCRYPTION.md](../../docs/AT_REST_ENCRYPTION.md), [docs/CONFIGURATION.md](../../docs/CONFIGURATION.md)  
**Related:** [e2e-message-crypto](../e2e-message-crypto/) (wire E2E; E008 PSK-at-rest), [chat-storage-and-memory](../chat-storage-and-memory/) (D048 plaintext transcripts)

## One-line goal

Protect profile secrets on disk with a PIN-wrapped DEK (libsodium Argon2id + XChaCha20-Poly1305) and durable whole-file writes (tmp → fsync → rename).

## Release scope (v1)

| In | Out |
|----|-----|
| `AtomicFileWrite` for JSON/blob stores | SQLCipher / `thread.db` encryption |
| `vault.bin` + mandatory PIN unlock | OS keychain unlock convenience |
| Encrypted `identity.enc` | Encrypting contacts/prefs wholesale |
| Encrypted PSK columns in `profile.db` | PIN recovery / multi-device vault sync |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Threat model, DEK/PIN model, file formats |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |
| [PHASES.md](PHASES.md) | Delivery checklist |
| [DECISIONS.md](DECISIONS.md) | ADRs (A001+) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| a0 | Atomic file writes | Done |
| a1 | Vault + FileCipher primitives | Done |
| a2 | Identity.enc + bootstrap PIN gate | Done |
| a3 | PSK column encryption | Done |
| a4 | Normative docs + cross-links | Done |
