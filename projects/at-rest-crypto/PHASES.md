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
- [x] `MessagingHub` create/unlock vault; `Bootstrap`/`--pin`/`PP_BROWSER_PIN`
- [x] Test fixtures inject DEK

## a3 — PSK at rest

- [x] Encrypt `master_psk_b64` + retired ledger entries under DEK
- [x] Update messaging crypto tests

## a4 — Docs

- [x] `docs/AT_REST_ENCRYPTION.md`
- [x] Update `CONFIGURATION.md`
- [x] Supersede E008 deferred note; cross-link D048
