# Current state — at-rest crypto

| Capability | Status | Notes |
|------------|--------|-------|
| `AtomicFileWrite` | **Yes** | `src/base/data/AtomicFileWrite.*` |
| JSON writers atomic | **Yes** | Config, prefs, registry, manifest, contacts, JsonThreadStore |
| `PinKeyDeriver` / `FileCipher` / `DataKeyVault` | **Yes** | `src/base/crypto/` |
| `ProfileSecretsService` | **Yes** | Vault unlock, DEK fan-out, Change PIN |
| `IDekConsumer` registry | **Yes** | `ProfileSecretsService::RegisterDekConsumer`; identity + PSK + transcript store |
| `EnsureMessagingReady` | **Yes** | `MessagingHub` after profile unlock |
| PIN GUI gate | **Yes** | `PinGateController` + shell overlay |
| Three-way chooser (A007) | **Yes** | After identity fork **I'm new**: Set PIN / default / Not now |
| Identity fork (M012) | **Yes** | I'm new vs I already have an account on first secrets use |
| Link-device vault wrap | **Yes** | `CreateWithDek` / `ReplaceWithDek`; shared DEK; account KEM copied (**M015**) |
| Default PIN + `pin_is_default` | **Yes** | `PinDefaults.h`; `preferences.json` schema v3 |
| Silent unlock (default PIN) | **Yes** | Bootstrap + `PromptUnlockIfVaultExists` |
| Change PIN (Settings) | **Yes** | Me → Security; `DataKeyVault::ChangePin` |
| Defer create / early unlock | **Yes** | A006; default-PIN path per A007 |
| `--pin` / `PP_BROWSER_PIN` | **Optional** | Tests/CI only |
| `identity.enc` | **Yes** | Under DEK after unlock |
| PSK columns encrypted | **Yes** | After DEK set |
| `thread.db` encrypted | **Yes** | D102 — `content_enc`, `value_enc`, `preview_enc` under profile DEK |
| OS keychain | **No** | Deferred |

**Wipe note:** Pre-D102 plaintext `thread.db` / `profile.db` (preview column) are incompatible — delete `{profile}/threads/` or the whole profile directory and recreate with a PIN.
