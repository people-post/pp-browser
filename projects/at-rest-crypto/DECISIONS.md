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
**Superseded in part by:** A009 (registry owner moved off ConversationsHub).

## A009 — ProfileSecretsEngine (app-wide vault owner)

**Date:** 2026-07-12  
**Decision:** Move `vault.bin`, PIN unlock, and `IDekConsumer` fan-out from `ConversationsHub` to `ProfileSecretsEngine` (`base/crypto`). Bootstrap initializes the profile service before the hub. `ConversationsHub::EnsureMessagingReady()` loads identity and starts libp2p/P2P after `ProfileSecretsEngine::IsUnlocked()`. UI/settings use the profile service for vault state and Change PIN; messaging features use `IsMessagingReady()`.  
**Rationale:** Profile PIN/DEK is app-wide infrastructure, not messaging-specific; enables future non-messaging encrypted stores without depending on the hub.  
**Alternatives:** Keep vault in ConversationsHub with `IDekConsumer` only (A008); fat `Application` owner (rejected — app wires, base owns domain).

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
4. **PIN recovery / cloud vault backup** remain out of scope. *Amended by [M023](../multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold):* recovery is in scope: a **recovery seed** the account keys derive from — kept by the user as a phrase, or (easy tier, the default) in the platform's synced keychain — plus an encrypted account locker. PIN-wrapped keys on a server, the device-only Keychain backup (A011) and custodial escrow stay out.
5. Which ciphertext blobs sync under the shared DEK (account ML-DSA, **account KEM**, public PSKs, etc.) is owned by multi-device-account — private PSKs excluded by default (E025 / **M015**).

**Rationale:** Per-device vault ≠ per-device master key; minimizes on-disk share while enabling one secrets realm.  
**Alternatives:** Distinct DEK per device; clone identical `vault.bin` bytes.
---

## A011 — Local account backup and restore unsupported in the product

**Date:** 2026-10-29  
**Status:** Accepted (Hongwei, 2026-10-29); **code removed** 2026-10-30 (M023 s5): `LocalAccountBackup`, the backup blob codec, the hub's backup write / restore, the gate's "Restore my account" and the probe's `--backup-store` / `--restore-backup` are gone; the store interface moved to `lib/people/SecureBlobStore` and the cleanup below stays. Restore is from the recovery phrase ([ACCOUNT_LOCKER.md](../../docs/contracts/ACCOUNT_LOCKER.md)).  
**Decision:** The app configures no store for the local account backup (`ConversationsHub` defaults to `NoSecureBlobStore`): nothing is written to the iOS Keychain and a fresh install offers no "Restore my account". The code (`LocalAccountBackup`, its codec, the hub's restore steps) stays, unit-tested and driven in the lab by the probe's file store (`--backup-store`, `--restore-backup`), until restore is ready as a whole.  
**Rationale:** The Keychain item's only reader is restore; without it the item only costs. It held the account secret keys under a short PIN (default-PIN accounts: readable by anyone with the unlocked phone), outliving an uninstall the user expects to remove them, and a growing copy of app data (contacts, groups, card floors) the Keychain is not meant for. Being `ThisDeviceOnly`, it never covered a lost phone either.  
**Cleanup (temporary):** items earlier builds wrote are deleted once messaging is up (`RemoveRetiredKeychainBackup`, idempotent). The cleanup goes at app version **1.1.0** (`kRetiredKeychainBackupRemoveAt`): `retired_keychain_backup_test` fails from that version on, so the release bump that reaches it carries the removal. *Amended 2026-10-30 ([M023](../multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) §3):* the iOS Keychain store (`OsSecureBlob*`) **stays** — M023's easy recovery tier keeps the seed in the synced keychain; only the retired `account-backup` item's cleanup and `LocalAccountBackup`'s use of it go.  
**Next:** decided in [M023](../multi-device-account/DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) — a recovery phrase as the root (never on a server), account state (contacts, groups, card floors) in an encrypted, signed account locker any provider can hold. `LocalAccountBackup` is not re-enabled; its contents become locker snapshots.

## A012 — Proposed: a per-device key replaces the public default PIN

**Date:** 2026-11-02  
**Status:** Proposed (Kenneth) — awaiting Hongwei.  
**Amends:** [A007](#a007--three-way-pin-chooser-default-pin-silent-unlock) (silent unlock keeps working; the key behind it changes), [A010](#a010--shared-dek-per-device-vault-wrap-multi-device) (the per-device wrap gains a device key).

1. **What.** "直接继续" (no PIN) stops using `kDefaultProfilePin = "123456"`. Each install generates a random 32-byte **device key** kept in the OS keychain (Keychain / Android Keystore / DPAPI / libsecret) and the vault stores `wrap(DEK, KEK_device)`; silent unlock reads the key, the user types nothing. Setting a PIN adds `KEK_pin = KDF(PIN, salt)` and the device wrap becomes `wrap(DEK, KEK_device ⊕ KEK_pin)`, so a vault file alone is not enough even with the PIN. Changing a PIN no longer asks a no-PIN user for a "current PIN".
2. **Why.** B86 / security audit #10: the default PIN is a public constant in a public repo, so every "直接继续" vault opens for anyone who copies the file; and a vault plus a 4-digit PIN is brute-forced offline in seconds. With the device key an offline attack also needs the keychain item, which never leaves the device. The user experience of "no PIN" is unchanged; the product is unreleased, so no migration of old vaults (A011 style).
3. **Agree?** If yes, we implement it as a PR for your review: `ProfileSecretsEngine` wrap/unwrap, one keychain adapter per platform behind the existing secure-blob port, chooser copy. Nothing on the wire changes.

## A013 — PIN grace window: a PIN typed a short while ago is not asked again

**Date:** 2026-11-06  
**Status:** Accepted (Kenneth) — client design; the change to [push P003](../push-notifications/DECISIONS.md#p003--pin-locked-generic-notify) is noted to Hongwei in the PR. First phase of the device-key work ([A012](#a012--proposed-a-per-device-key-replaces-the-public-default-pin) stays proposed).  
**Amends:** [A007](#a007--three-way-pin-chooser-default-pin-silent-unlock) (a custom PIN is not asked at every cold start), [push P003](../push-notifications/DECISIONS.md#p003--pin-locked-generic-notify).

1. **What.** After a PIN unlock the DEK is kept for a window the user picks in Me → Security (off / 5 / 15 / 60 minutes, default 15): `grace.bin` (the DEK under a random `K_grace`) and a keychain entry (`K_grace` + the window's end), never the PIN or the DEK in the clear ([AT_REST_ENCRYPTION § PIN grace window](../../docs/contracts/AT_REST_ENCRYPTION.md#pin-grace-window-gracebin--a-keychain-entry)). The gate reads it after the default PIN and before the dev PIN / unlock screen.
2. **Counted from the last use in the foreground.** Using the app keeps extending it (entering / leaving the foreground, every minute in it); background activity — a push wake, a background sync — may unlock through an open window but never extends it, so a phone left locked overnight asks for the PIN in the morning. No overall cap.
3. **Device clocks.** The end is a point on the monotonic clock of the current boot; another boot reads as expired, the system time does not matter.
4. **Locked phone.** iOS keeps the entry `WhenUnlockedThisDeviceOnly`: with the phone locked it cannot be read and nothing is deleted. A call answered on the locked phone shows "Unlock your phone to connect the call" (with "Enter PIN instead") and connects when the phone is unlocked, or falls back to the PIN screen after 3 minutes; without a call the PIN screen shows and the phone's unlock or the next foreground still opens it.
5. **Forgotten** on Change PIN, a profile reset, the window set to off, or expiry. Platforms without the keychain entry (desktop; Android until its store lands) behave as before.
6. **Why.** Kenneth 2026-11-06: typing the PIN at every cold start and every locked-phone call is the main friction of a custom PIN. **Risk accepted:** within a window, someone holding the unlocked phone opens PP (as when PP is still running today); expiry is judged by the app, so until it next runs a keychain dump of an unlocked phone yields the DEK. Offline attacks on `vault.bin` (audit #10) are unchanged.
