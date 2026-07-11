# Current state — at-rest crypto

| Capability | Status | Notes |
|------------|--------|-------|
| `AtomicFileWrite` | **Yes** | `src/base/data/AtomicFileWrite.*` |
| JSON writers atomic | **Yes** | Config, prefs, registry, manifest, contacts, JsonThreadStore |
| `PinKeyDeriver` / `FileCipher` / `DataKeyVault` | **Yes** | `src/base/crypto/` |
| `vault.bin` + `--pin` / `PP_BROWSER_PIN` | **Yes** | `MessagingHub::Initialize`, `Bootstrap` |
| `identity.enc` | **Yes** | Replaces plaintext `identity.json` |
| PSK columns encrypted | **Yes** | `SqlitePskSessionStore` encrypts under DEK |
| `thread.db` encrypted | **No** | D048 plaintext transcripts |
| PIN UI | **No** | CLI/env only until settings UI |
| OS keychain | **No** | Deferred |

**Wipe note:** Existing plaintext profiles are incompatible — delete the profile data directory and recreate with a PIN.
