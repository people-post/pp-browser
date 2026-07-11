# Decisions — at-rest crypto

## A001 — PIN-wrapped DEK vault (mandatory PIN)

**Date:** 2026-07-11  
**Decision:** Each profile has a random 32-byte DEK. PIN derives KEK via Argon2id (`crypto_pwhash`); DEK is wrapped with XChaCha20-Poly1305 into `vault.bin`. Unlock required before identity/PSK use. Forgotten PIN → wipe profile (no recovery key in v1).  
**Rationale:** PIN change re-wraps DEK without re-encrypting all payloads; matches agreed plan.  
**Alternatives:** PIN-as-direct-file-key; opt-in PIN; OS-keychain-only.

## A002 — Secrets-first scope (identity + PSK; not transcripts)

**Date:** 2026-07-11  
**Decision:** Encrypt `identity.enc` and PSK columns / retired PSK blobs in `profile.db`. Leave `thread.db` plaintext (chat-storage D048 remains). No SQLCipher in this project.  
**Rationale:** Highest leverage against disk theft of keys; transcript encryption is a larger product/ops change.  
**Alternatives:** Full DB encryption; encrypt all JSON stores.

## A003 — Atomic whole-file writes

**Date:** 2026-07-11  
**Decision:** All whole-file JSON/blob replaces go through `AtomicFileWrite` (same-dir tmp → fsync → rename). Not used for live SQLite page I/O.  
**Rationale:** Prevents truncated/corrupt finals on crash.  
**Alternatives:** Write-in-place; write under unrelated cache volume (rejected — cross-device rename).

## A004 — Vault and payload envelopes

**Date:** 2026-07-11  
**Decision:** `vault.bin` magic `PPBV`, version 1, little-endian KDF limits, 16-byte salt, 24-byte nonce, wrapped DEK ciphertext. Payload blobs use existing `EncryptedPayload` layout under DEK with AAD `purpose|profile_id|schema`.  
**Rationale:** Fixed binary layout; AAD prevents file swap across purposes/profiles.  
**Alternatives:** JSON vault; separate AEAD library.

## A006 — GUI PIN; defer create; early unlock if vault exists

**Date:** 2026-07-11  
**Decision:** PIN is collected in-app (blocking overlay). CLI/env PIN is optional for automation only. **No vault:** defer PIN create until `EnsureSecretsUnlocked` (first secrets use); create dialog may be cancelled (“Not now” / Escape). **Vault exists:** require unlock after UI load before secrets APIs (no cancel). Single hub API owns both paths.  
**Rationale:** Normal users need GUI; deferring create keeps local AI light; early unlock when vault exists avoids half-unlocked sync/E2E paths.  
**Alternatives:** Mandatory CLI PIN; defer unlock even when vault exists.