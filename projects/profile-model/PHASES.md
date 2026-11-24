# Profile model: phases

Order of work. Status is in [CURRENT_STATE.md](CURRENT_STATE.md). Each phase is a clean cutover (pre-release, RC017):
a profile of the old layout is not read.

## P0 — Design

- [x] Inventory of what is on disk and every service ([INVENTORY.md](INVENTORY.md)).
- [x] Decisions PM001–PM010 accepted; open questions 2–3 are defaults, 4–6 wait for several backends per profile
      ([DESIGN § Open questions](DESIGN.md#open-questions)).

## P1 — Networks and opaque profiles (PM001, PM003, PM007, PM008, PM009)

Tracked in [#752](https://github.com/people-post/pp-browser/issues/752).

- [ ] The manifest records `network`; written atomically when the folder is made; a folder without a readable
      manifest, or a newer one, is skipped and logged.
- [ ] Opaque ids; `profiles.json` keeps one active id per backend; no adoption, no folder named after a key.
- [ ] Each profile is made with its one `backends/{backend_id}/` folder; a missing pointer finds it by that folder.
- [ ] The guard reads the network: a tester app never opens `main`, `--profile` included.
- [ ] Reset my data makes a new profile on the same network and deletes the old folder.
- [ ] `machine.json` loses `active_profile_id`; pp-node gets its own data root and network.
- [ ] Tests: #733's three scenarios, reset then switch (INVENTORY § E 1, #738), a tester app given a `main` profile, a
      folder without a manifest.
- [ ] Closes #733 and #738.

## P2 — Backends as service tables (PM002, PM010)

Tracked in [#753](https://github.com/people-post/pp-browser/issues/753).

- [ ] A backend type: id, network, one URL (and relay id where it applies) per service.
- [ ] Built-in tables from the build; a custom table from `config.json`; no fallback to another backend's URL.
- [ ] Every caller takes its URL from the table (INVENTORY § Services, the one-origin list).
- [ ] The Network page edits a custom backend's whole table.

## P3 — Per-backend state (PM004)

Tracked in [#754](https://github.com/people-post/pp-browser/issues/754).

- [ ] `backends/{backend_id}/`: the registration slot and Brief LLM token out of `identity.enc` into
      `registration.enc`; client-compat, mesh node list and reputation, Home feed, locker state moved.
- [ ] The inbox cursor's "preferred" slot goes; per-relay records stay keyed by relay.

## P4 — Backend switch (PM005 as narrowed by PM009)

Tracked in [#755](https://github.com/people-post/pp-browser/issues/755).

- [ ] Every switch opens the new backend's profile (PM009); editing a custom backend's registration URL or org relay is
      a switch (PM010, #745).
- [ ] Push unbound on every relay with a binding; binding ids kept on disk.
- [ ] One "switch pending" rule for the banner and push.

## P5 — Machine-wide settings (PM006)

Tracked in [#756](https://github.com/people-post/pp-browser/issues/756).

- [ ] `relay.extra_base_urls` and the LLM / MCP URLs into the backend table; `initiation_floor` to the account;
      `show_tester_options` to `machine.json`.

## P6 — Promote

Tracked in [#757](https://github.com/people-post/pp-browser/issues/757).

- [ ] DATA_LAYOUT, SERVICE_ENDPOINTS, CONFIGURATION, BUILD § Backend origin, AT_REST_ENCRYPTION say the new layout;
      the drift in INVENTORY § E 14 fixed with them.
- [ ] RC022 points at the network guard.

## Later — Several backends per profile (after release 1)

- [ ] Open questions 4–6 answered; a same-network switch keeps the profile and points the new backend at it; the
      custom-backend confirm in a store app. No layout change.
