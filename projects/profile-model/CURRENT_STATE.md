# Profile model — current state

**As of:** 2026-11-24

P0: inventory written ([INVENTORY.md](INVENTORY.md)); PM001–PM012 accepted by Hongwei (2026-11-23). Release 1 builds the
whole layout; a profile has exactly one home backend (PM009, PM012). In [DESIGN § Open questions](DESIGN.md#open-questions), 2–3 are
defaults and 4–6 are closed by PM012 (one home backend per profile, permanently); P0 is done.
The issues found in the inventory are #738–#751; the phases are tracked in #752–#757.

**P2a (backend tables) is built** (PR D, `kenneth/pm-backend-tables`; nothing else of P2): the backend type
(`BackendTable`), the built-in tables compiled from `CompiledOrigins`, a custom table read from `config.json` with optional
keys only (`backend.services`; `config_version` stays 1), every caller taking its address from the active table, a service
the table lacks absent with no fallback, and PM010's custom id as a function that profile selection does not use yet.
Closed: #742 and #743. #741 and #744 as far as the table goes (a custom backend no longer takes prod's LLM or MCP; a user's
promoted-MCP address is kept when it carries `set_by_user`, and a derived seed or any unmarked leftover is replaced by the backend's seed and never written back); the `services.json` half of #744 (the
Integrations field editing a per-profile setting) is P2b. Not done in P2a: `services.json`, the Network page's table editor and
the page binding of the service list (P2b); no consumer yet reads `mesh_nodes` or `seed_relays`; the `/api/relay` mount in
`HttpBlobClient` / `RelayFileUrl` / `RelayOrigins`, `HttpClient`'s `kUiBlockRoutes` and `CrashReportEnvelope` do not take
addresses from the table (they parse relay-list addresses, labels, or a caller's URL).

**P1 (networks and opaque profiles) is built** (branch `hongwei/profile-model-p1`, #752; merges after #758 and #760):
opaque profile ids, a manifest of schema 2 recording the network, `profiles.json` schema 2 with one pointer per backend,
each profile made with its one `backends/{backend_id}/` folder, the network guard, Reset my data making a new profile
in-process, `--profile` as a label, the app removing old-format profiles with their keychain items, pp-node on its
own data root and network with its old profile converted in place; `machine.json` lost `active_profile_id`. Contract:
[DATA_LAYOUT § Per-backend profiles](../../docs/contracts/DATA_LAYOUT.md#per-backend-profiles). A custom backend's id is
PM010's (`CustomBackendId`), so its folder under `backends/` follows the registration URL and the org relay.

**P3 (per-backend state) is built** (local branch `hongwei/profile-model-rest`, #754): `backends/{backend_id}/`
holds `registration.enc` (the registration slot and the Brief LLM token, out of `identity.enc`), `client_compat.json`
and `account_locker.json`; the inbox cursor file keeps every relay's cursor by origin. Left: the mesh caches keyed by
origin (pp-cpp-mesh writes `mesh_directory_nodes.json`).

**P5 (machine-wide settings) is built** (same branch, #756): seed relays from the backend's table, `show_tester_options`
in `machine.json`. `initiation_floor` stays the config dogfood seed (pricing P001 item 7) until there is a Me screen
for it.

**P4 (backend switch) is built** (same branch, #755): one pending rule (`BackendSwitchPending`), push unbound on every
relay, a custom backend's registration URL or org relay edited is a switch (banner, unbind, not applied live).

**P2b is built** (same branch, #753): `services.json` per profile (the user's promoted MCP moved there out of
`config.json`; directory providers and mesh node source as overrides), the Network page's editor for a custom backend's
whole table, and the line saying which features it leaves off. Not built: a consumer for the mesh node override.
