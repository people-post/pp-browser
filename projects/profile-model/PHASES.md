# Profile model: phases

Order of work. Status is in [CURRENT_STATE.md](CURRENT_STATE.md). Each phase is a clean cutover (pre-release, RC017):
a profile of the old layout is not read.

## P0 — Design

- [x] Inventory of what is on disk and every service ([INVENTORY.md](INVENTORY.md)).
- [x] Decisions PM001–PM012 accepted; open questions 2–3 are defaults, 4–6 closed by PM012
      ([DESIGN § Open questions](DESIGN.md#open-questions)).

## P1 — Networks and opaque profiles (PM001, PM003, PM007, PM008, PM009)

Tracked in [#752](https://github.com/people-post/pp-browser/issues/752).

- [x] The manifest records `network`; written atomically when the folder is made; a folder without a readable
      manifest, or a newer one, is skipped and logged.
- [x] Opaque ids; `profiles.json` keeps one active id per backend; no adoption, no folder named after a key.
- [x] Each profile is made with its one `backends/{backend_id}/` folder; a missing pointer finds it by that folder.
- [x] The guard reads the network: a tester app never opens `main`, `--profile` included.
- [x] Reset my data deletes the old folder and makes a new profile on the same backend and network, re-opened
      in-process.
- [x] `machine.json` loses `active_profile_id`; pp-node gets its own data root and network, its old profile
      converted in place (identity kept).
- [x] `--profile NAME` is a label among the running backend's profiles; the app removes old-format profiles with
      their keychain items.
- [x] Tests: #733's three scenarios, reset then switch (INVENTORY § E 1, #738), a tester app given a `main` profile, a
      folder without a manifest.
- [ ] Closes #733 and #738.

## P2 — Backends as service tables (PM002, PM010, PM011)

Tracked in [#753](https://github.com/people-post/pp-browser/issues/753).

- [x] A backend type: id, network, one URL (and relay id where it applies) per operator service, and the seeds
      (PM011). (P2a: `BackendTable`.)
- [ ] `profiles/{id}/services.json`: directory providers, mesh node list source, promoted MCP, seeded from the
      backend when the profile is made; `directory.providers` and `promoted_mcp` leave `config.json`.
- [x] Built-in tables from the build; a custom table from `config.json`; no fallback to another backend's URL. (P2a.)
- [x] Every caller takes its URL from the table (INVENTORY § Services, the one-origin list). P2a did the operator services' callers (`DeploymentProfile`, `Config`, `PlatformDefaults`, the hub's clients, token renew, the LLM mint / renew, the crash upload). The rest pick no backend address and stay as they are (Hongwei, 2026-11-24): the `/api/relay` mount in `HttpBlobClient` / `RelayFileUrl` / `RelayOrigins` parses relay-list addresses, `HttpClient` `kUiBlockRoutes` are labels, `CrashReportEnvelope` takes its caller's URL. `mesh_nodes` and `seed_relays` get their consumers in P2b / P5.
- [ ] The Network page edits a custom backend's whole table.

## P3 — Per-backend state (PM004)

Tracked in [#754](https://github.com/people-post/pp-browser/issues/754).

- [x] `backends/{backend_id}/`: the registration slot and Brief LLM token out of `identity.enc` into
      `registration.enc` (AAD names profile and backend; an older identity.enc gives them up at its next save);
      `client_compat.json` and `account_locker.json` moved (once, at the first start after P3). The mesh node list, its
      reputation and the Home feed stay at the profile's top level (PM011).
- [ ] Their caches keyed by origin: `mesh_directory_nodes.json` is written by pp-cpp-mesh (`MeshDirectoryCache`), so
      it waits for a mesh change; with one home backend per profile (PM012) it holds one source's list anyway.
- [x] The inbox cursor's "preferred" slot goes: every relay's cursor is written by its origin; an older file's
      `cursor` / `cursor_relay` are read once.

## P4 — Backend switch (PM005 as narrowed by PM009)

Tracked in [#755](https://github.com/people-post/pp-browser/issues/755).

- [ ] Every switch opens the new backend's profile (PM009); editing a custom backend's registration URL or org relay is
      a switch (PM010, #745).
- [ ] Push unbound on every relay with a binding; binding ids kept on disk.
- [ ] One "switch pending" rule for the banner and push.

## P5 — Machine-wide settings (PM006)

Tracked in [#756](https://github.com/people-post/pp-browser/issues/756).

- [x] `relay.extra_base_urls` into the backend's seeds: a custom backend's table carries them; a built-in backend seeds
      its own (none) and ignores the file's; under `PP_BROWSER_ORIGIN` the file's still seed.
- [x] The Brief LLM URLs are the table's (P2a); a user's own LLM endpoint stays in `config.json` (a device setting).
- [x] `show_tester_options` to `machine.json` (an optional key); the profile preference is gone (a tester taps the
      version five times once more).
- [ ] `initiation_floor` to the account: **kept** as the config dogfood seed for now — pricing P001 item 7 makes it the
      only way to set the floor until a Me screen exists; it only seeds the account's own value while that is 0.

## P6 — Promote

Tracked in [#757](https://github.com/people-post/pp-browser/issues/757).

- [ ] DATA_LAYOUT, SERVICE_ENDPOINTS, CONFIGURATION, BUILD § Backend origin, AT_REST_ENCRYPTION say the new layout;
      the drift in INVENTORY § E 14 fixed with them.
- [ ] RC022 points at the network guard.

## Later — Home move (possible, after release 1)

- [ ] Design and decide a home move (PM012): a profile to another backend of its network with the same identity —
      register at the new org relay, move the locker, revoke the old registration; the custom-backend confirm in a
      store app. No layout change.
