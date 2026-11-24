# Profile model: inventory

Everything the app keeps on a device and every service it calls, at main `6f64150d6` (2026-11-23), read from the
code and checked against [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md) and
[SERVICE_ENDPOINTS](../../docs/contracts/SERVICE_ENDPOINTS.md). **Layer** is the one [DESIGN](DESIGN.md#model) puts it
in: root (machine), **N** network, **A** account, **D** device profile, **B** backend state, **R** relay state.
Line numbers drift; the names do not.

## A. Root (machine-wide)

| Path | Holds | Layer | Notes | Written |
|---|---|---|---|---|
| `config.json` | `backend.env`, relay / directory / registration URLs and relay ids (custom only), `relay.extra_base_urls`, `data_dir`, mesh, LLM, promoted MCP, MCP servers, `initiation_floor` | root | Selects the backend; extras, floor, LLM and MCP leak into whichever profile runs (PM006) | `Config.cpp` via `SessionStore`; keys `ConfigJson.cpp` |
| `profiles.json` | schema 1, `active_profile_id`, `profiles[{id, label}]` | root | The real active profile | `ProfileRegistry::Save` |
| `machine.json` | window, display, safe area, run / crash log switches, update card; `active_profile_id` | root | Its `active_profile_id` is written, never read (stays `default`); DATA_LAYOUT calls it live | `UserPreferences.cpp`; keys `ConfigJson.cpp` |
| `logs/`, `diagnostics/crash_pending.txt` | run logs; last fatal-signal dump | root | The crash upload goes to the running backend's relay, gated by a profile preference | `LogFile.cpp`, `CrashDump.cpp` |
| Cache dir (per platform) | made empty, never used | root | Not in DATA_LAYOUT | `AppPaths.cpp` |
| iOS `pp-browser-debug.log` | iOS debug log, outside `data_dir` | root | Not in DATA_LAYOUT | `PlatformLogSink.cpp` |

## B. Per profile (`profiles/{id}/`)

**One backend per profile** marks the files that assume it.

| Path | Holds | Keyed by | One backend per profile | Layer | Written |
|---|---|---|---|---|---|
| `manifest.json` | schema, `backend` | profile | **yes** | N | `ProfileManifest.cpp` |
| `identity.enc` | account keys, Account ID, recovery seed, nickname, floor (A); device ML-DSA and KEM, card intent (D); registration slot (`registered`, `registered_backend_url` — really the preferred relay's URL — `registration_id`, `registered_card_issued_at`, `kem_binding_published`) and Brief LLM token and guest key (B) | AAD `identity\|{id}` | **yes**: one registration slot, one LLM token | A / D / B | `IdentityStore.cpp` |
| `own_card.enc` | our newest account card | AAD `own-card\|{id}` | no | A | `IdentityStore.cpp` |
| `vault.bin`, `grace.bin`, `biometric.bin` | the DEK's PIN wrap; grace and biometric seals | AAD names `{id}` | no | D (the DEK itself is the account's, A010) | `DataKeyVault.cpp`, `SealedDek.cpp` |
| `secrets.enc` | typed LLM API key | AAD names `{id}` | no | D | `ConfigSecretsStore.cpp` |
| `our_relays.json` | the account's relays, preferred first, each with its registration record | relay origin + relay id | seeded once from the running backend | A (list) / R (records) | `OurRelaysStore.cpp` |
| `relay_pins.json` | expected relay id per origin, highest certificate per relay id | origin, relay id | no (org pins use the build's relay id) | R | `RelayPinStore.cpp` |
| `relay_inbox_cursor.json` | poll cursors, registrations, reread state | one account; a "preferred" slot plus `relays{origin}` | **partly** | R | `RelayInboxCursor.cpp` |
| `client_compat.json` | cached `GET /v1/client-compat` (support, crash URL, certificate, switches) | nothing | **yes** | B | `ClientCompat.cpp` |
| `mesh_directory_nodes.json` | last-good `GET /v1/mesh/nodes` | nothing | **yes** | B | path `ProductMesh.cpp` (writer in pp-cpp-mesh) |
| `mesh_node_reputation.json` | demoted nodes and operators | node, operator | implicitly | B | `MeshNodeReputation.cpp` |
| `mesh_last_good_relays.json` | up to 3 mesh relay PeerIds | nothing | implicitly | D | `LastGoodRelayStore.cpp` |
| `home_brief.json` | Home article feed | nothing | **yes** | B | `HomeBrief.cpp` |
| `account_locker.json` | locker versions, contact clock, device names | locker id | the locker is on the org's registration URL | B | `AccountLockerWorkflow.cpp` |
| `contacts.json` | address book; `remote` part from the directory | contact, account | no | D (remote part a cache) | `ContactsStore.cpp` |
| `preferences.json` | schema 16 | profile | `auto_renew_registration` | D (`show_tester_options` → root, PM006) | `UserPreferences.cpp` |
| `initiation_billing.json`, `payment_promises.json` | per-peer billing; promises | peer account | relay floor kept per peer | D | `InitiationBillingStore.cpp`, `PaymentPromiseStore.cpp` |
| `attachment_suppressions.json` | deleted attachment hashes | thread | no | D | `AttachmentSuppressionStore.cpp` |
| `cache/icons/` | profile photos | account | no | D | `ProfileIconCache.cpp` |
| `threads/profile.db`, `threads/{id}/thread.db` | chats, outbox, sessions, cards, groups, calls, posts | thread, account, device | no | D | `SqliteThreadSchema.h` |
| `cas/`, `object_index.db`, `blob_cipher/`, `picked/` | attachment bytes, staging, previews | content, thread | no | D | `CasStore.cpp`, `OutgoingAttachmentStore.cpp`, `ChatController.cpp` |

## C. Outside `data_dir`

| Where | Item | Layer |
|---|---|---|
| iOS keychain, service `dev.pp-browser.secure-blob` | `{id}/unlock_grace`, `{id}/unlock_biometric` (`{id}` is the folder name, `ProfileSecretsEngine.cpp`) | D |
| iOS keychain | `account-backup` (retired, only deleted) | root |
| Android keystore | `pp_secure_blob_when_unlocked`, `pp_secure_blob_after_first_unlock`; entries as files under `noBackupFilesDir/secure_blob/` | root / D |
| Android SharedPreferences | `pp_browser_device`: `device_id` (the push device id for every profile and backend), `message_banners_on`; `pp_keep_alive.last_ms` (not in DATA_LAYOUT) | root |
| iOS | push device id = `identifierForVendor` | root |
| Desktop | no secure store | — |
| Push bindings | only on the relays; binding ids in memory | B / R |

## D. What each action deletes

| Action | Deletes | Keeps |
|---|---|---|
| **Reset my data** (`Application::ResetActiveProfile`) | Remote first: push unbind (another device remains) or registration revoke on every relay (only device). Then grace and biometric items, the whole profile folder; the manifest is written again **without** a backend | `profiles.json`, `config.json`, `machine.json`, logs, other profiles, Android preferences and keystore keys, the push device id |
| **Reset chats** (`ResetChatData`) | `threads/` only; public sessions are kept, `restart_pending` | Everything else; `relay_inbox_cursor.json` is not rewound, `cas/` is orphaned |
| **Device removed** (main device's `RemoveOwnDevice`) | nothing on either device; the removed one stops sending and publishing, and its "relink" is Reset my data | — |
| **Link import / restore** | `own_card.enc` (`ReplaceDekKeepLoaded`) | — |
| **Backend switch** | nothing local; push unbound on the **preferred relay only** | The old profile, intact |

## Services

How each is reached today. prod, staging and dev derive relay, directory and registration as `origin + /api/relay`,
the LLM as `origin + /api/llm/v1`, MCP as `origin + /mcp` (`Config.cpp` `ApplyBackendOverride`); custom uses its saved
relay, directory and registration URLs, and everything else from the fallback environment's origin.

| Service | Paths | Base URL from | Per | State |
|---|---|---|---|---|
| Relay messages | `/v1/messages`, `/v1/inbox/poll`, `ack`, `clear` | the account's relays (preferred first; sends to the recipient's) | relay | `relay_inbox_cursor.json`, `our_relays.json` |
| Stream history | `/v1/streams/messages/query` | our relay at the org registration URL while ours, else the preferred | backend | — |
| Registration | register start / finish, revoke, nickname | each of our relays | relay | `identity.enc` slot (preferred), `our_relays.json` |
| Directory | `/v1/users/by-account/{id}`, search | configured directory, then each of our relays | backend, then relay | `peer_cards` |
| Mesh nodes | `/v1/mesh/nodes` | the first directory | backend | `mesh_directory_nodes.json` |
| Push devices | `/v1/devices/register`, `unregister` | preferred relay plus each relay with our registration | relay | none on disk |
| Profile photos | `/v1/cdn/presign`, `retain`, `delete`, `/v1/profile/icon` | preferred relay | relay | `cache/icons/` |
| Attachments | `/v1/blob/start`, `part`, `complete`, `delete`, `list` | preferred relay; downloads from the URL in the message | relay | `attachment_suppressions.json` |
| Account lockers | `/v1/lockers/put`, `GET /v1/lockers/:id` | the org's registration URL | backend | `account_locker.json` |
| Relay identity | `/v1/relay-identity` | each of our relays and the org origins | relay | `relay_pins.json` |
| Client compat (and support, crash URL, certificate, switches) | `/v1/client-compat` | org relay | backend | `client_compat.json` |
| Crash reports (dormant) | `/v1/crash-reports` or the compat `crash_reports_url` | org relay | backend | `diagnostics/` |
| Brief LLM | `chat/completions`, `/pp/chat/stream`, `/guest/start` | `origin + /api/llm/v1`, forced for the `brief` preset | backend | `identity.enc` token |
| LLM token | issued at the org relay's `register/finish`; renewed at the registration origin's `/api/llm/v1/token/renew` | registration origin | backend | `identity.enc` |
| Promoted MCP | `origin + /mcp` | origin; a user's value is overwritten on prod, staging and dev | backend | `home_brief.json` |
| Mesh bootstrap | compiled seeds | not the origin | machine | — |
| pp-node heartbeat | `/v1/mesh/heartbeat` | `registration.base_url`, default prod | node | node `relay_pins.json` |

**Code that assumes one origin for every service:** `DeploymentProfile.cpp` (`BriefOrigin()` plus fixed paths,
`CompiledEnvFor`), `Config.cpp` `ApplyBackendOverride`, `PlatformDefaults.cpp`, `OrgBackendClientsImpl.cpp` (token
renew under the registry's origin), `LlmPreset.cpp`, `HttpBlobClient.cpp` / `RelayFileUrl.cpp` / `RelayOrigins.cpp`
(the `/api/relay` mount), `HttpClient.cpp` `kUiBlockRoutes`, `CrashReportEnvelope.cpp`, `BackendProfile.cpp`
(`custom-<hash of registration URL>`), the Network page's three custom fields.

## E. Found on the way

Read from the code, not run, except where a test is named. Each to be filed or fixed on its own.

1. **Reset, then a backend switch before the restart:** the next start claims the reset folder for the new backend;
   switching back finds no profile for the old one and cannot make `profiles/{key}` — the app does not start on it
   (#733 by another route).
2. **`machine.json` `active_profile_id` is dead** (above).
3. **Every DEK-sealed file and the keychain items are bound to the folder name;** pp-node with `--profile` gives
   `IdentityStore` the registry's id while its secrets engine takes the folder name.
4. **A custom backend uses the fallback environment's LLM and MCP** (prod's in a store app; `CompiledEnvFor`), while its
   LLM token is issued and renewed at the custom origin.
5. **A custom relay may inherit the fallback's relay id** when the file names none (defaults applied before the latch,
   kept by the merge).
6. **`directory.providers[]` in the config overrides the compiled directory**, prod included.
7. **A user's promoted-MCP URL is overwritten** on prod, staging and dev, and written back.
8. **Editing custom URLs while on custom** applies them live under the old profile, leaves the preferred relay on the
   old URL, unbinds push, and opens a new profile on the next start.
9. **A backend switch unbinds push on the preferred relay only;** binding ids are in memory only.
10. **"Switch pending" is decided twice:** the banner compares environments, push compares profile keys.
11. **`show_tester_options` is per profile** but decides which backends are offered; a new profile starts with it off.
12. **The machine-wide push device id** is shared by every profile and backend; a reset keeps it.
13. **pp-node shares the desktop app's data root and `profiles.json`** by default and never selects by backend.
14. **Documentation drift:** DATA_LAYOUT lacks `threads/{id}/blob_cipher/`, `picked/preview/`, several `profile.db`
    tables (calls, posts, `device_sessions`, `control_lane_seen`, `group_member_seq`, `group_targets`), preferences
    past schema 14, `ai_dot_look`'s current values, Android SharedPreferences and several `identity.enc` keys; it still
    lists the root `mesh_directory_nodes.json` and says `threads/{id}/blobs/` is unused (it is still made).
    SERVICE_ENDPOINTS still lists the removed diagnostics upload and cites `OrgBackendClientFactory` (no non-test
    caller). CONFIGURATION § backend.env describes loading a file without `backend` as `custom`, which no code does.
    M023 / A010 mention a synced-keychain tier for the recovery seed that does not exist.
