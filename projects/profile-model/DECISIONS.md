# Profile model: decisions

## PM001 — A network on top; the tester-app guard is at the network

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23). Built; the outcome is in [DATA_LAYOUT § Per-backend profiles](../../docs/contracts/DATA_LAYOUT.md#per-backend-profiles). Amends
[RC022](../release-compatibility/DECISIONS.md#rc022--tester-builds-never-touch-production-profiles-hard-guard): the
guard reads the profile's network, not its backend key.
**Decision:**
- A **network** is a world whose accounts never meet another's: `main` (the real one), `staging`, `dev`, and
  `test-custom` (a tester app's custom backend). pp-node's profile is on `node` ([PM007](#pm007--pp-node-has-its-own-network-and-its-own-data-root)).
- Every profile records its network in its manifest, written when the profile is made and never changed.
- A store app opens `main`, `staging` and `dev` profiles; a tester app opens `staging`, `dev` and `test-custom`, never
  `main`. The check runs before anything in the profile is read.
- A store app's custom backend is on `main` (a real user's data, out of the tester app's reach); for release 1 it
  has a profile of its own (PM009). A tester app's custom backend is on `test-custom`.
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
different server with different url"). Built. Narrowed by
[PM011](#pm011--a-backend-holds-only-the-operators-services-the-rest-are-profile-settings-seeded-from-it): the table
holds only the operator's services; directory, mesh node list and promoted MCP are profile settings it seeds.
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
**Status:** Accepted (Hongwei, 2026-11-23). Built; the outcome is in [DATA_LAYOUT § Per-backend profiles](../../docs/contracts/DATA_LAYOUT.md#per-backend-profiles).
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
**Status:** Accepted (Hongwei, 2026-11-23). Built; the outcome is in [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md) (`backends/`), [AT_REST_ENCRYPTION § Registration plaintext](../../docs/contracts/AT_REST_ENCRYPTION.md#registration-plaintext-inside-backendsbackend-idregistrationenc). Built before release 1, with one backend per profile
([PM009](#pm009--one-backend-per-profile-for-release-1-in-the-final-layout)). Narrowed by
[PM011](#pm011--a-backend-holds-only-the-operators-services-the-rest-are-profile-settings-seeded-from-it): the mesh
node list, its reputation and the Home feed follow profile settings and stay at the profile's top level.
**Decision:**
- What the device knows about one backend's services goes in `profiles/{id}/backends/{backend_id}/`: the org
  registration slot and the Brief LLM token (moved out of `identity.enc`, sealed under the DEK with the backend id in
  the AAD), client-compat, the mesh node list and node reputation, the Home feed, the account locker's sync state.
- What it knows about one relay stays keyed by that relay wherever it is kept today (`our_relays.json` registration
  records, `relay_pins.json`, `relay_inbox_cursor.json`), and the cursor's single "preferred" slot goes.
- Account data (account keys, Account ID, recovery seed, own card) and device data (device key, vault wrap, chats,
  contacts, preferences) do not depend on the backend and stay where they are.
**Rationale:** The inventory found the one-backend assumption in a short, specific list of files (INVENTORY § B); moving
those under the backend's id keeps them apart from the rest of the profile, and lets a later home move
(PM012) hold two backends' state for a while without mixing them.

## PM005 — A backend switch keeps the profile within a network, changes it across networks, and unbinds push everywhere

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23: "Backend switch may also be redesigned if needed"). Built. Narrowed
for release 1 by [PM009](#pm009--one-backend-per-profile-for-release-1-in-the-final-layout): every switch opens that
backend's own profile. Its keeping the profile within a network, and the privacy confirm, are now part of a possible
home move ([PM012](#pm012--a-profile-has-exactly-one-home-backend-permanently)).
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
**Status:** Accepted (Hongwei, 2026-11-23). Built; the outcome is in [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md) (`machine.json`), [CONFIGURATION](../../docs/ops/CONFIGURATION.md) (`relay.extra_base_urls`). Amended by
[PM011](#pm011--a-backend-holds-only-the-operators-services-the-rest-are-profile-settings-seeded-from-it): the
promoted-MCP URL and `directory.providers` are profile settings.
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
**Status:** Accepted (Hongwei, 2026-11-23). Built.
**Decision:** pp-node's profile records network `node`, and pp-node uses a data root of its own by default, not the
desktop app's.
**Rationale:** pp-node shares the desktop app's data root and `profiles.json` today unless `data_dir` is set, takes the
registry's active id as its AAD profile id while its secrets engine takes the folder name, and never selects a profile
by backend.

## PM008 — One account per install for release 1; the layout allows more

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23: "We do only one account for now, just to make the design ready"). Built.
Amended the same day with [PM009](#pm009--one-backend-per-profile-for-release-1-in-the-final-layout): the pointer is per
backend, not per network.
**Decision:**
- `profiles.json` lists profiles and keeps one active profile id per **backend** (`active: {backend_id → profile
  id}`); the manifest stays the only record of a profile's network.
- Release 1 makes and opens one profile per backend and shows no account switcher. A second account on a backend is a
  second profile and a change of that backend's pointer; nothing else in the layout changes for it.
**Rationale:**
- One pointer per backend means a tester app and a store app on one install never overwrite each other's choice.
- Each backend points at its own profile, and the profile is found again by its `backends/{backend_id}/` folder if
  the pointer is lost. A later switcher needs no new layout either.

## PM009 — One backend per profile for release 1, in the final layout

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23: "only support one backend for now and expand more support later, the file
layout are expected to be the final version upfront"). Built. Narrows PM005 for release 1. Made permanent by
[PM012](#pm012--a-profile-has-exactly-one-home-backend-permanently): a profile never uses several backends; the "later feature" below is replaced by a home move.
**Decision:**
- The whole layout of [DESIGN § On disk](DESIGN.md#on-disk) is built before release 1, `backends/{backend_id}/`
  included, with the registration slot and the Brief LLM token out of `identity.enc` (PM004).
- For release 1 a profile has exactly one `backends/{backend_id}/` folder, made with the profile. A backend switch,
  within a network or across, opens that backend's profile, or makes one.
- ~~Letting a second backend of the same network into a profile is a later feature.~~ Superseded by PM012.
- If a backend's pointer is missing or stale, the registry takes the profile on the backend's network that holds
  `backends/{backend_id}/`; if there is none, it makes one.
**Rationale:**
- The layout is free to change before release 1 (RC017) and costs a migration after; the behaviour can grow later.
- With one backend per profile, the questions about sharing an account across backends (the org relay in the relay
  list, which locker, which history) wait until that feature ([DESIGN § Open questions](DESIGN.md#open-questions)).
- A store app's custom backend keeps an identity of its own, so the account is never registered with another
  deployment without the user asking.
**Alternatives rejected:** keep the per-backend files where they are now and move them later (a migration after release
1 for a layout we already know).

## PM010 — A custom backend is identified by its registration URL and its org relay

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23). Built. Answers DESIGN open question 1.
**Decision:**
- A custom backend's id is `custom-<hash>` (`test-custom-<hash>` in a tester app) over its registration URL and its org
  relay's origin and relay id, normalized. The id names its `backends/` folder and is in the AAD of that folder's
  sealed files.
- Changing the registration URL or the org relay is a backend switch: a confirm, push unbound, and that backend's
  profile on the next start.
- Changing any other service of the table (account locker, client-compat, LLM, crash intake) keeps the backend and its
  profile and applies in place.
**Rationale:** The account lives at the registration URL and the org relay: its registration, its token and the relay
it seeds the list with. The other services can move without touching the account. Today an edit of the registration
URL makes a new profile on the next start without saying so (#745).

## PM011 — A backend holds only the operator's services; the rest are profile settings seeded from it

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23: "shrink backend definitions … so that it only holds services that are no
longer split"). Built; the outcome is in [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md) (`services.json`), [CONFIGURATION](../../docs/ops/CONFIGURATION.md) (`promoted_mcp`). Narrows PM002, PM004 and PM006.
**Decision:**
- A backend's services are only those that need the account's registration there, or are the operator's own:
  registration and org relay (its identity, PM010), account locker, the Brief LLM token and the gateway that takes it,
  client-compat (support, relay certificate, switches), crash intake.
- The directory providers, the mesh node list source and the promoted MCP are **profile settings** in
  `profiles/{id}/services.json`: seeded from the backend's defaults when the profile is made, the user's after that —
  as the relay list is (`our_relays.json`, seeded from the org relay and the seed relays).
- A user's own LLM endpoint stays a device preference.
- So no service is ever taken from another backend: there is nothing to compose. A backend's table also carries the
  seeds (relays, directory providers, mesh node list, promoted MCP), which only a new profile reads.
- State that follows a profile setting stays at the profile's top level, keyed by the service's origin where it caches
  an answer: `mesh_directory_nodes.json`, `mesh_node_reputation.json`, `home_brief.json`.
**Rationale:**
- The services an account does not depend on can be anyone's: cards verify against the Account ID, the node list and
  the feed tools are public. Making them the user's own settings removes the need to mix backends, which PM002 would
  otherwise have needed.
- `config.json` (shared by every build) and the profile's files freeze at release 1, so the split is made now.
**Alternatives rejected:** custom tables that take a service `from` another backend of the same network (more rules —
same network only, custom only, explicit — for what profile settings give without them).

## PM012 — A profile has exactly one home backend, permanently

**Date:** 2026-11-23
**Status:** Accepted (Hongwei, 2026-11-23). Built. Makes PM009's rule permanent; closes DESIGN open questions 4–6.
**Decision:**
- A profile's backend is its account's **home operator** on its network: exactly one, for the profile's life. A profile
  never uses two backends at once.
- **Being reachable at several operators is the relay list's job** (multi-relay MR002–MR006): the account registers on
  up to four relays and is delivered to on each. A backend's org relay is one entry in that list, seeded when the
  profile is made.
- So the questions a shared profile would have raised are settled: the org relay is seeded into the relay list (as
  today); the account locker is the home backend's; stream history is read from the home backend's org relay while it
  is ours (as today).
- **A home move** — taking a profile to another backend of its network with the same identity (register at the new
  org relay, move the locker, revoke the old registration) — is a possible later feature, not designed now. The layout
  needs no change for it: during a move the profile holds two `backends/` folders, kept apart by the backend id in
  their AAD, then one again. Until then a switch opens the other backend's own profile (PM009), so a store app's custom
  backend has an identity of its own unless the user moves home.
**Rationale:** After PM011 a backend holds only what depends on the account's registration there: its registration,
one locker, one LLM token, the operator's policy. Each of those wants one owner. What a second backend would add —
being registered and reachable elsewhere — the relay list already does.
**Alternatives rejected:** several backends per profile (PM009's later feature): it needs rules for which locker and
which history, for what the relay list gives.

## PM013 — The ledger network is a choice of its own, pinned by network card

**Date:** 2026-11-27
**Status:** Accepted (Hongwei, 2026-11-27). Not built: the app has no ledger client yet; pp-node's ledger role already
works this way. The screens' words are Kenneth's (default: below).
**Decision:**
- A **ledger network** — pp-ledger's devnet, testnet or mainnet
  ([pp-ledger RL009](https://github.com/people-post/pp-ledger/blob/main/projects/release/DECISIONS.md), or a chain
  anyone runs) — is not one of the app's networks (PM001) and not a service of a backend (PM002): a third-party
  network the profile chooses on its own, by its **network card** (network id and genesis hash), never by an address
  alone.
- **Ours by default, any by choice.** The app ships the cards of pp-ledger's mainnet — the network new ledger accounts
  are made on — and its testnet. A user may add any other network by its card, as a deliberate step that shows its
  genesis fingerprint and says it is not one of ours. A link may propose a network; it never switches one.
- **On our mainnet, an account in one step:** the app's onboarding makes and pays for a new user's account there.
  Elsewhere the user brings their own.
- **A tester build opens only our test networks** (testnet, devnet), and adds none: it never opens pp-ledger's mainnet,
  as it never opens `main` (PM001), and it cannot tell which other network carries value.
- **A successor of our mainnet** (pp-ledger's re-genesis, after its keys were stolen or lost) comes in an app update,
  its card the new default, with our announcement. Opening it, the app says it continues the user's accounts once its
  import checks out against the old network (the user's accounts proven on both, as pp-ledger's `check-predecessor`
  checks a whole ledger for nodes): that proves the state carried over, not who runs it; trust comes from the
  announcement and the app's default.
- **A ledger key belongs to one ledger network:** kept under its network id, it never signs for another, so nothing
  made for a test network spends on the real one.
- **The ledger network is always shown** on every screen that reads or signs for it; a test network says so plainly
  (default words: "Testnet — tokens have no value").
- **pp-node:** its ledger role's network is its own pp-ledger `config.json` (upstreams, `networkAnchor`), not the
  node's backend; a fleet picks it per environment.
**Rationale:**
- pp-ledger's testnet runs for months before its mainnet: meanwhile the store app's prod backend (and a prod node's
  ledger role) must use the testnet, the only public chain. Tying the ledger network to the app's network would force
  prod to a mainnet that does not exist yet, and later stop a prod user trying the testnet.
- The chain is not ours to bind: anyone may run a network, and the app is open source, so a mandate would bind only
  our build. Org services are defaults, not dependencies; our network wins by being the default and the best served
  (an account in one step), not the only one. No signed network list: nothing for us to guard, or for a thief to take.
- What must never happen is real value in a tester build, or a test key spending real tokens: the guard and the keys
  per network say exactly that.
**Alternatives rejected:** the ledger network from the app's network (`main` → mainnet, `staging` → testnet): wrong
during the testnet's soak, and it makes a third party's chain part of our backend. Our mainnet mandatory, or only
networks on a list we sign: an open-source app can be rebuilt without either, and a list's key is one more thing a
thief of the chain's keys would want.

