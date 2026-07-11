# Current state — at-rest crypto

| Capability | Status | Notes |
|------------|--------|-------|
| `AtomicFileWrite` | **Yes** | `src/base/data/AtomicFileWrite.*` |
| JSON writers atomic | **Yes** | Config, prefs, registry, manifest, contacts, JsonThreadStore |
| `PinKeyDeriver` / `FileCipher` / `DataKeyVault` | **Yes** | `src/base/crypto/` |
| `EnsureSecretsUnlocked` | **Yes** | Hub API; create or unlock |
| PIN GUI gate | **Yes** | `PinGateController` + shell overlay |
| Defer create / early unlock | **Yes** | A006 |
| `--pin` / `PP_BROWSER_PIN` | **Optional** | Tests/CI only |
| `identity.enc` | **Yes** | Under DEK after unlock |
| PSK columns encrypted | **Yes** | After DEK set |
| `thread.db` encrypted | **No** | D048 plaintext transcripts |
| OS keychain | **No** | Deferred |

**Wipe note:** Existing plaintext profiles are incompatible — delete the profile data directory and recreate with a PIN.
