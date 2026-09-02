# Decisions — at-rest crypto

## A001 — PIN-wrapped DEK vault (mandatory PIN)

**Date:** 2026-07-11  
**Decision:** Each profile has a random 32-byte DEK. PIN derives KEK via Argon2id (`crypto_pwhash`); DEK is wrapped with XChaCha20-Poly1305 into `vault.bin`. Unlock required before identity/PSK use. Forgotten PIN → wipe profile (no recovery key in v1).  
**Rationale:** PIN change re-wraps DEK without re-encrypting all payloads; matches agreed plan.  
**Alternatives:** PIN-as-direct-file-key; opt-in PIN; OS-keychain-only.

## A002 — Secrets-first scope (identity + PSK + transcripts)

**Date:** 2026-07-11  
**Updated:** 2026-08-19 — transcript bodies/previews/memory encrypted (chat-storage **D102**).  
**Decision:** Encrypt `identity.enc`, PSK columns / retired PSK blobs in `profile.db`, and transcript content columns (`content_enc`, `preview_enc`, `value_enc`) under the profile DEK. No SQLCipher. Transcript metadata (timestamps, titles, sync watermarks) stays plaintext.  
**Rationale:** Highest leverage against disk theft of keys and message text; avoids full-DB encryption ops.  
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
**Updated:** 2026-08-13 — identity fork precedes the chooser (M012).  
**Decision:** On first secrets use when no `vault.bin` exists, show **I'm new on this device** vs **I already have an account** first. **I'm new** then shows the three-way chooser: **Set a PIN** (custom create flow), **Just continue** (auto-create vault with `kDefaultProfilePin` = `123456` from `src/foundation/crypto/PinDefaults.h`), or **Not now** (defer). **I already have an account** uses the same PIN choices for *this* device, then paste a link payload into an empty vault (shared DEK; [M012](../multi-device-account/DECISIONS.md#m012--link-device-ritual-deferred-until-m4)). Persist `pin_is_default: true` in `preferences.json` (schema v3) when the default path is chosen; clear it on **Change PIN** in Me → Security (`DataKeyVault::ChangePin`). When `pin_is_default` is true, bootstrap and `PromptUnlockIfVaultExists` attempt silent unlock with the default PIN — no blocking modal unless unlock fails. Custom-PIN profiles keep A006 blocking unlock. Show a one-time toast after default provisioning pointing to Me → Security. `--pin` / `PP_BROWSER_PIN` still take the create path.  
**Rationale:** Reduces friction for users who want E2E/register without choosing a PIN upfront; keeps encryption on by default while making stronger protection opt-up via Settings. `pin_is_default` avoids Argon2 probing on every startup for custom-PIN users. Link-device must not mint a Brief person before wrapping the shared DEK.  
**Alternatives:** Hardcoded default without flag; nag on every secrets action; truly unprotected (no vault) mode; in-place Security import (rejected — M012).

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

---

## A010 — Shared DEK; per-device vault wrap (multi-device)

**Date:** 2026-08-11  
**Amends:** README “multi-device vault sync” out-of-scope — **shared DEK + link seal** is now in scope under [multi-device-account](../multi-device-account/) (not a second vault product).  
**Canonical:** [multi-device-account M004](../multi-device-account/DECISIONS.md#m004--shared-dek-per-device-vault-wrap).  
**Cross-project:** [D099](../chat-storage-and-memory/DECISIONS.md#d099--account-id-amends-d096-multi-device), [e2e E025](../e2e-message-crypto/DECISIONS.md#e025--account-envelope-signing--private-psk-not-auto-synced).

**Decision:**

1. Linked devices share one **DEK** (account secrets realm).
2. Each install keeps its own **`vault.bin`** — PIN-derived wrap of that DEK (PIN may differ per device).
3. Link-device seals the DEK to the new install; the new install wraps into its vault (A001 layering unchanged: PIN wraps DEK, DEK encrypts payloads).
4. **PIN recovery / cloud vault backup** remain out of scope.
5. Which ciphertext blobs sync under the shared DEK (account ML-DSA, **account KEM**, public PSKs, etc.) is owned by multi-device-account — private PSKs excluded by default (E025 / **M015**).

**Rationale:** Per-device vault ≠ per-device master key; minimizes on-disk share while enabling one secrets realm.  
**Alternatives:** Distinct DEK per device; clone identical `vault.bin` bytes.