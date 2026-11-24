# Profile model: design

**Status:** Proposed (2026-11-23), decided in outline ([PM001](DECISIONS.md#pm001--a-network-on-top-the-tester-app-guard-is-at-the-network)–[PM008](DECISIONS.md#pm008--one-account-per-install-for-release-1-the-layout-allows-more)); nothing built. Pre-release: a clean cutover, no migration ([RC017](../release-compatibility/DECISIONS.md#rc017--p0s-decisions-a-clean-local-start-the-currency-id-fixed-two-features-after-release-1)). What holds today is in [DATA_LAYOUT § Per-backend profiles](../../docs/contracts/DATA_LAYOUT.md#per-backend-profiles) until this is built and promoted.

## Why

A profile today is a backend's: `profiles/{backend key}/`, its manifest records the key, and a build picks the profile
for the backend it runs. Three things follow that are wrong:

- **A profile can lose its owner.** Reset my data writes the manifest again without a backend, and whichever build
  starts next claims it; a folder named after a key can then not be made for that key again, and the app exits at
  launch ([#733](https://github.com/people-post/pp-browser/issues/733); the same by a backend switch before a restart,
  INVENTORY § E 3).
- **An account cannot span backends,** although it is the user's own key (multi-relay MR001) and its relays are its own
  list (MR002). A store app's custom backend gets a separate identity.
- **A backend is one origin.** Every service is the origin plus a fixed path, so a custom backend uses prod's LLM and
  MCP and may expect prod's relay id (INVENTORY § Services).

## Model

Four layers, each belonging to exactly one above it:

| Layer | Is | Kept |
|---|---|---|
| **Network** | `main`, `staging`, `dev`, `test-custom`, `node`: worlds whose accounts never meet (PM001) | The profile's manifest |
| **Account** | The user's identity: account ML-DSA and KEM keys, Account ID, recovery seed, own account card, initiation floor, relay list. Spans the account's devices | `identity.enc` (account part), `own_card.enc`, `our_relays.json` (list) |
| **Device profile** | This install in that account: device key, the vault's wrap of the account's DEK, chats, contacts, preferences, caches | `profiles/{id}/` |
| **Backend and relay state** | What this device knows of one backend's services, or one relay | `profiles/{id}/backends/{backend_id}/` (PM004); per-relay records keyed by relay |

The **tester-app guard** is at the top: a tester app never opens a `main` profile, whatever it is asked to open
(`--profile` included).

A **backend** is not a layer of the profile: it is a table of services (PM002) the app runs against. Its network says
which profiles may use it.

## Networks and builds

| Network | Store app | Tester app | pp-node |
|---|---|---|---|
| `main` | yes: prod and the user's custom backends | **never** | — |
| `staging` | yes (behind the tester options) | yes | — |
| `dev` | yes (behind the tester options) | yes | — |
| `test-custom` | — | yes: the tester's custom backends | — |
| `node` | — | — | yes |

## Backends

```
backend
  id         prod | staging | dev | custom-<opaque>
  network    main | staging | dev | test-custom
  services
    org_relay        url, relay_id
    directory        url (and failover providers)
    registration     url, relay_id
    account_locker   url
    client_compat    url
    llm_gateway      url
    llm_token        url
    promoted_mcp     url
    crash_intake     url
    mesh_nodes       url
    support          (from client-compat today)
  seed_relays        the extra relays a new profile's list starts with
```

- Built-in tables come from the build (today's `CompiledOrigins`, one per environment, filled in from the one origin).
- A custom backend's table is the user's, kept in `config.json`; a service the user did not give is absent, and what
  needs it is off (no fallback to a built-in backend's URL).
- The active backend is one id in `config.json`; the network comes from the backend. A build that cannot run the saved
  backend runs its fallback and keeps the saved choice (as today, RC022).

## On disk

```
{data_dir}/
  config.json                  # machine: the active backend id, custom backends' tables, device-wide settings
  machine.json                 # machine: window, display, logs; show_tester_options (PM006); no active profile
  profiles.json                # index: profiles [{id, label}], active: {network → profile id} (PM008)
  profiles/
    {id}/                      # opaque, fixed for life (PM003)
      manifest.json            # schema_version, network — written first, atomically, never changed
      identity.enc             # account keys + device key (no registration slot, no LLM token)
      own_card.enc, vault.bin, grace.bin, biometric.bin, secrets.enc
      our_relays.json          # the account's relay list + per-relay registration records
      relay_pins.json, relay_inbox_cursor.json   # per relay; the cursor's "preferred" slot goes
      contacts.json, preferences.json, threads/, cas/, cache/, …   # device data, unchanged
      backends/
        {backend_id}/
          registration.enc     # the org registration slot + Brief LLM token, sealed, AAD names profile and backend
          client_compat.json
          mesh_directory_nodes.json, mesh_node_reputation.json
          home_brief.json
          account_locker.json
```

The manifest is the only record of a profile's network; `profiles.json` holds pointers and labels, never the network,
so the two cannot disagree. At startup the registry reads `profiles/*/manifest.json`.

## Startup

1. Load `config.json`; resolve the active backend (or this build's fallback) and its network.
2. Refuse a network this build may not open (the guard; a `--profile` override is checked the same way).
3. Read the manifests. Take the network's pointer from `profiles.json`; if it is missing, or names a profile that is
   gone or on another network, take the first profile on the network; if there is none, make one.
4. Open `backends/{backend_id}/` in it, making it on first use.

A folder without a readable manifest, or with a newer one, is logged and left alone. Nothing is ever claimed or
renamed.

## Reset my data

Make a new profile on the same network (the first-start path), point the network at it, then delete the old folder
with its keychain items. The remote steps before it stay as today (unbind push, revoke the registrations of an only
device).

## Backend switch

- **Same network:** the profile stays; the active backend changes; push is unbound on every relay holding a binding
  of ours.
- **Other network:** the other network's profile opens (or is made) on the next start; push is unbound the same way.
- Push binding ids are kept on disk (in the backend's or relay's record) so a restart can still unbind them.
- Onto a store app's custom backend: the confirm says the account is registered with that deployment.

## Open questions

1. **A custom backend's identity across edits.** An opaque id that the user's edits keep, or a new backend when its
   registration URL changes? (Today an edit makes a new `custom-<hash>` profile on the next start, without a confirm.)
2. **The org relay and the account's relay list on a switch within `main`.** Is the new backend's org relay added to
   the list, offered, or left out?
3. **Account lockers** stay at the org's registration URL (MR006). With one account on several backends, which
   backend's locker is the account's?
4. **Stream history** is read from the org's relay while it is ours (`HistoryRelay`): per backend, or per relay?
5. **Switch without a restart.** The backend is latched once per process today.
6. **`node`.** Whether pp-node's profile needs a network at all once its data root is its own.

## Not in scope

- An account switcher UI (PM008: the layout allows it).
- Changes to the relay list, the account card or the device list (multi-relay, multi-device-account).
