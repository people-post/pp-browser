# Phases — at-rest crypto

## a0 — Atomic writes

- [x] `AtomicFileWrite` in `base/data`
- [x] Switch JSON writers (config, prefs, registry, manifest, contacts, JsonThreadStore)
- [x] Unit tests

## a1 — Vault primitives

- [x] `PinKeyDeriver` (Argon2id)
- [x] `FileCipher` (DEK AEAD + AAD)
- [x] `DataKeyVault` (`vault.bin` create/unlock/change-pin/lock)
- [x] Frozen/round-trip tests

## a2 — Identity + unlock gate

- [x] `identity.enc` under DEK
- [x] Rename inner field to `private_key_b64`
- [x] `ConversationsHub` create/unlock vault; `Bootstrap`/`--pin`/`PP_BROWSER_PIN`
- [x] Test fixtures inject DEK

## a3 — PSK at rest

- [x] Encrypt `master_psk_b64` + retired ledger entries under DEK
- [x] Update messaging crypto tests

## a4 — Docs

- [x] `docs/contracts/AT_REST_ENCRYPTION.md`
- [x] Update `CONFIGURATION.md`
- [x] Supersede E008 deferred note; cross-link D048

## a5 — GUI unlock / deferred create (A006)

- [x] `ConversationsHub::Initialize` without PIN; `EnsureSecretsUnlocked`
- [x] `PinGateController` + shell overlay
- [x] Early unlock when vault exists; defer create until secrets use
- [x] Gate Register / Secure message / PSK actions
- [x] CLI/env PIN optional for automation only

## a6 — Three-way chooser + default PIN (A007)

- [x] Chooser overlay: Set a PIN / Just continue / Not now (full-width vertical stack)
- [x] `kDefaultProfilePin` + `pin_is_default` in `preferences.json` (schema v3)
- [x] Silent unlock at bootstrap and `PromptUnlockIfVaultExists` when `pin_is_default`
- [x] Me → Security section: protection status + Change PIN
- [x] Docs + ADR A007

## a7 — IDekConsumer registry (A008)

- [x] `IDekConsumer` (`SetDek` / `ClearDek`) in `base/crypto`
- [x] `IdentityStore` + `SqlitePskSessionStore` implement the interface
- [x] Consumer registration on `ProfileSecretsService` (moved from hub in a8)
- [x] Docs + ADR A008

## a8 — ProfileSecretsService (A009)

- [x] `ProfileSecretsService` in `base/crypto` — vault, unlock, DEK fan-out
- [x] `ConversationsHub::EnsureMessagingReady` (libp2p/P2P after profile unlock)
- [x] `PinGateController`, `Bootstrap`, Settings → profile service; messaging checks → hub
- [x] Docs + ADR A009
