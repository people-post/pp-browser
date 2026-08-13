# Current state — at-rest crypto

| Capability | Status | Notes |
|------------|--------|-------|
| `AtomicFileWrite` | **Yes** | `src/base/data/AtomicFileWrite.*` |
| JSON writers atomic | **Yes** | Config, prefs, registry, manifest, contacts, JsonThreadStore |
| `PinKeyDeriver` / `FileCipher` / `DataKeyVault` | **Yes** | `src/base/crypto/` |
| `ProfileSecretsService` | **Yes** | Vault unlock, DEK fan-out, Change PIN |
| `IDekConsumer` registry | **Yes** | `ProfileSecretsService::RegisterDekConsumer`; identity + PSK |
| `EnsureMessagingReady` | **Yes** | `MessagingHub` after profile unlock |
| PIN GUI gate | **Yes** | `PinGateController` + shell overlay |
| Three-way chooser (A007) | **Yes** | After identity fork **I'm new**: Set PIN / default / Not now |
| Identity fork (M012) | **Yes** | I'm new vs I already have an account on first secrets use |
| Default PIN + `pin_is_default` | **Yes** | `PinDefaults.h`; `preferences.json` schema v3 |
| Silent unlock (default PIN) | **Yes** | Bootstrap + `PromptUnlockIfVaultExists` |
| Change PIN (Settings) | **Yes** | Me → Security; `DataKeyVault::ChangePin` |
| Defer create / early unlock | **Yes** | A006; default-PIN path per A007 |
| `--pin` / `PP_BROWSER_PIN` | **Optional** | Tests/CI only |
| `identity.enc` | **Yes** | Under DEK after unlock |
| PSK columns encrypted | **Yes** | After DEK set |
| `thread.db` encrypted | **No** | D048 plaintext transcripts |
| OS keychain | **No** | Deferred |

**Wipe note:** Existing plaintext profiles are incompatible — delete the profile data directory and recreate with a PIN.
