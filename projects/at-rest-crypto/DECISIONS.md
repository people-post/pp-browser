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
**Decision:** PIN is collected in-app (blocking overlay). CLI/env PIN is optional for automation only. **No vault:** defer PIN create until `EnsureSecretsUnlocked` (first secrets use); create dialog may be cancelled (“Not now” / Escape). **Vault exists (custom PIN):** require unlock after UI load before secrets APIs (no cancel). Single hub API owns both paths.  
**Rationale:** Normal users need GUI; deferring create keeps local AI light; early unlock when vault exists avoids half-unlocked sync/E2E paths.  
**Alternatives:** Mandatory CLI PIN; defer unlock even when vault exists.  
**Superseded in part by:** A007 (three-way chooser; silent unlock for default PIN).

## A007 — Three-way PIN chooser; default PIN; silent unlock

**Date:** 2026-07-12  
**Decision:** On first secrets use when no `vault.bin` exists, show a three-way chooser: **Set a PIN** (custom create flow), **Just continue** (auto-create vault with `kDefaultProfilePin` = `123456` from `src/base/crypto/PinDefaults.h`), or **Not now** (defer). Persist `pin_is_default: true` in `preferences.json` (schema v3) when the default path is chosen; clear it on **Change PIN** in Me → Security (`DataKeyVault::ChangePin`). When `pin_is_default` is true, bootstrap and `PromptUnlockIfVaultExists` attempt silent unlock with the default PIN — no blocking modal unless unlock fails. Custom-PIN profiles keep A006 blocking unlock. Show a one-time toast after default provisioning pointing to Me → Security.  
**Rationale:** Reduces friction for users who want E2E/register without choosing a PIN upfront; keeps encryption on by default while making stronger protection opt-up via Settings. `pin_is_default` avoids Argon2 probing on every startup for custom-PIN users.  
**Alternatives:** Hardcoded default without flag; nag on every secrets action; truly unprotected (no vault) mode.

## A008 — IDekConsumer registry for DEK fan-out

**Date:** 2026-07-12  
**Decision:** At-rest stores that need the profile DEK implement `IDekConsumer` (`SetDek` / `ClearDek`). Registry and fan-out live on the profile secrets service (see A009).  
**Rationale:** Avoids hand-wiring each new encrypted store into unlock; lock zeroing stays consistent.  
**Alternatives:** Keep manual `identity_->SetDek` / `psk_store_->SetDek` calls; shared DEK pointer (rejected — copies keep lifetime simple and match existing test injection).  
**Superseded in part by:** A009 (registry owner moved off MessagingHub).

## A009 — ProfileSecretsService (app-wide vault owner)

**Date:** 2026-07-12  
**Decision:** Move `vault.bin`, PIN unlock, and `IDekConsumer` fan-out from `MessagingHub` to `ProfileSecretsService` (`base/crypto`). Bootstrap initializes the profile service before the hub. `MessagingHub::EnsureMessagingReady()` loads identity and starts libp2p/P2P after `ProfileSecretsService::IsUnlocked()`. UI/settings use the profile service for vault state and Change PIN; messaging features use `IsMessagingReady()`.  
**Rationale:** Profile PIN/DEK is app-wide infrastructure, not messaging-specific; enables future non-messaging encrypted stores without depending on the hub.  
**Alternatives:** Keep vault in MessagingHub with `IDekConsumer` only (A008); fat `Application` owner (rejected — app wires, base owns domain).