# Profile model: decisions

## PM001 — A network on top; the tester-app guard is at the network

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23). Not built. Amends
[RC022](../release-compatibility/DECISIONS.md#rc022--tester-builds-never-touch-production-profiles-hard-guard): the
guard reads the profile's network, not its backend key.
**Decision:**
- A **network** is a world whose accounts never meet another's: `main` (the real one), `staging`, `dev`, and
  `test-custom` (a tester app's custom backend). pp-node's profile is on `node` ([PM007](#pm007--pp-node-has-its-own-network-and-its-own-data-root)).
- Every profile records its network in its manifest, written when the profile is made and never changed.
- A store app opens `main`, `staging` and `dev` profiles; a tester app opens `staging`, `dev` and `test-custom`, never
  `main`. The check runs before anything in the profile is read.
- A store app's custom backend is on `main`: the same account as prod. A tester app's custom backend is on
  `test-custom`.
**Rationale:**
- An account is the user's own key (multi-relay [MR001](../multi-relay/DECISIONS.md#mr001--inboxes-are-keyed-by-account-id)),
  so nothing ties a profile to one backend. What must stay apart is the test networks from the real one, and from each
  other.
- The guard then asks one question of one field, and the field cannot go missing (PM003).
**Alternatives rejected:**
- A profile per backend (today): the same account cannot use two backends, and the backend key in the folder name and
  manifest is what #733 broke on.
- Only a "visible to the tester app" flag: it protects user data but says nothing about which profile a backend opens.

## PM002 — A backend is a named table of services, each with its own URL

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23: "a group of services provided by remote servers, each service can be on a
different server with different url"). Not built.
**Decision:**
- A **backend** has an id, a network (PM001) and a table of services, each with its own URL and, where the service is
  a relay, the relay id it must prove: org relay, directory, registration, account locker, client-compat, LLM gateway,
  LLM token service, promoted MCP, crash intake, mesh node list, support.
- The built-in backends (`prod`, `staging`, `dev`) are tables the build compiles. That every service of a built-in
  backend sits under one origin today is how those tables are filled in, not a rule any code relies on.
- A custom backend is a table the user enters; it has no service the user did not enter (no fallback to another
  backend's URL).
- **A backend is not a relay.** The account's relays are its own list (multi-relay MR002); a backend's org relay only
  seeds that list for a new profile and is where org services live (multi-relay
  [MR006](../multi-relay/DECISIONS.md#mr006--the-preferred-relay-carries-the-users-relay-services-org-services-stay-on-the-orgs-origin)).
**Rationale:** Today one origin plus fixed paths stands for every service (`Config.cpp` `ApplyBackendOverride`), so a
custom backend sends its LLM and MCP calls to prod's origin and may expect prod's relay id. Naming each service makes
the table the one place a URL comes from.
**Alternatives rejected:** keep one origin per backend and derive the rest (the custom backend cannot be described
correctly, and every new service adds another fixed path).

## PM003 — A profile id is opaque and fixed for the profile's life; a reset makes a new profile

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23). Not built.
**Decision:**
- A profile's folder is `profiles/{id}/`, `{id}` a random opaque name, never derived from a network or backend.
- The id never changes: every DEK-sealed file's AAD and the keychain items (`{id}/unlock_grace`,
  `{id}/unlock_biometric`) name it.
- A profile is never unmarked: its manifest (network, schema version) is written atomically when the folder is made,
  before anything else goes in. A folder with no readable manifest, or a newer one, is skipped and logged, never
  claimed.
- **Reset makes a new profile** on the same network, through the same path as a first start, and the old folder is
  deleted. There is no path that rewrites a manifest.
**Rationale:**
- #733: the reset rewrote the manifest without its backend, the next build to start claimed the folder, and a folder
  named after a backend key could then not be made again. Opaque ids and no adoption leave nothing to claim and no
  name to collide on.
- The AAD binding (INVENTORY § Surprises) means a profile could never be renamed safely anyway.
**Alternatives rejected:** folders named by backend key (what is broken today); keeping the reset's rewrite but writing
the key (fixes one route, leaves adoption and the name clash).

## PM004 — Per-backend state lives in the device profile, under `backends/{backend_id}/`

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23). Not built.
**Decision:**
- What the device knows about one backend's services goes in `profiles/{id}/backends/{backend_id}/`: the org
  registration slot and the Brief LLM token (moved out of `identity.enc`, sealed under the DEK with the backend id in
  the AAD), client-compat, the mesh node list and node reputation, the Home feed, the account locker's sync state.
- What it knows about one relay stays keyed by that relay wherever it is kept today (`our_relays.json` registration
  records, `relay_pins.json`, `relay_inbox_cursor.json`), and the cursor's single "preferred" slot goes.
- Account data (account keys, Account ID, recovery seed, own card) and device data (device key, vault wrap, chats,
  contacts, preferences) do not depend on the backend and stay where they are.
**Rationale:** The inventory found the one-backend assumption in a short, specific list of files (INVENTORY § B); moving
those under the backend's id is what lets one account use several backends of its network.

## PM005 — A backend switch keeps the profile within a network, changes it across networks, and unbinds push everywhere

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23: "Backend switch may also be redesigned if needed"). Not built.
**Decision:**
- Switching to a backend on the same network keeps the profile (the same account, chats and contacts) and changes the
  active backend; its state under `backends/{backend_id}/` is made on first use.
- Switching to a backend on another network opens that network's active profile, or makes one.
- Either way push is unbound on every relay that holds a binding of ours, not only the preferred one, and the binding
  ids are kept on disk so a restart can still unbind them.
- Whether a switch still needs a restart is left open ([DESIGN § Open questions](DESIGN.md#open-questions)).
- A store app's switch onto a custom backend says, before saving, that the account's identity is registered with that
  deployment.
**Rationale:** Today the switch unbinds the preferred relay only and keeps binding ids in memory, so the other relays
keep pushing to a device that has left (INVENTORY § E 8).

## PM006 — Machine-wide config holds only machine-wide settings

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23). Not built.
**Decision:**
- `relay.extra_base_urls` belongs to the backend (it seeds a new profile's relay list from that backend's table).
- `initiation_floor` belongs to the account (it is already a field of the account card).
- The LLM and promoted-MCP URLs belong to the backend's table (PM002); a user's own LLM endpoint is a device
  preference.
- `show_tester_options` moves from the profile's preferences to `machine.json`: it decides which backends the app
  offers, before a profile is open.
- `machine.json` loses its `active_profile_id` (written, never read).
**Rationale:** These are read by whichever profile runs today, so one profile's value leaks into another, and a new
profile starts with the tester options off.

## PM007 — pp-node has its own network and its own data root

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23). Not built.
**Decision:** pp-node's profile records network `node`, and pp-node uses a data root of its own by default, not the
desktop app's.
**Rationale:** pp-node shares the desktop app's data root and `profiles.json` today unless `data_dir` is set, takes the
registry's active id as its AAD profile id while its secrets engine takes the folder name, and never selects a profile
by backend.

## PM008 — One account per install for release 1; the layout allows more

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23: "We do only one account for now, just to make the design ready"). Not built.
**Decision:**
- `profiles.json` lists profiles and keeps one active profile id per network; the manifest stays the only record of a
  profile's network.
- Release 1 makes and opens one profile per network and shows no account switcher. A second account on a network is a
  second profile and a change of that network's pointer; nothing else in the layout changes for it.
**Rationale:** One pointer per network means a tester app and a store app on one install never overwrite each other's
choice, and a later switcher needs no new layout.
