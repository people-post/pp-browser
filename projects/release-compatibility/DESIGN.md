# Release compatibility: design

**Status:** Accepted by Hongwei, 2026-11-17; in review with Leo (relay parts: [§ Decisions taken](#decisions-taken),
items 3 and 8). Decisions RC001–RC016.
**Today's policy** stays [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md) until release 1 ships; this
design replaces its [§ No bridges](../../docs/contracts/COMPATIBILITY.md#no-bridges-pre-release) then.

## The rule

> **From release 1 on, every app, pp-node and relay works with every other one whose version is inside the
> supported range.**
>
> - Each kind of component has a supported range of versions: the current release back to the oldest one still
>   supported.
> - Any two components whose versions are both in range work together. A feature only the newer one knows may be
>   unavailable, but the older one says so; nothing fails silently.
> - Breaking changes are still allowed. A breaking change may drop support only for versions that are leaving the
>   range. It never breaks two versions that are both still in it.
> - A version older than the range is refused cleanly, with "update needed".
> - Release 1 is the first build under this rule. Everything before it is outside the range.

### Terms

- **Supported range:** which versions of each kind of component are guaranteed to work together. It is declared
  and published (the [version table](#the-version-table)), not implied.
- **Breaking change:** a change after which the new version cannot work with some older one. Allowed only when that
  older version is already leaving the range.
- **Leaving the range:** a version stops being supported. It is announced, then enforced by raising the relay's
  `min_client_version` (or the node floor).
- **Format:** anything two separately released components exchange or store: a wire message, a signed object, an
  HTTP request or response, a stored record.

## Why

Pre-release, every change was a clean cutover (2026-11-12): no code for older peers, wire forms or data, and every
app and node updated together. That was chosen to move fast, and it worked while one team shipped everything at
once. It does not fit the product:

- Phones update when their owners update them. pp-node runs on users' own hardware. The relay deploys on its own
  schedule, and multi-relay means relays run by others. These parts never move in lockstep.
- The repo has many version numbers (below), each designed carefully but alone. Nothing says which combinations of
  app, node and relay work together.
- No component can roll back: every cutover writes data its previous release refuses (account card v4,
  `profile.db` 7 → 8). A relay revert to the time of TestFlight 136 (2026-11-17) had no clean answer.
- Mismatches surface inconsistently. Some say "update" (relay `426`, an unknown `envelope_version`); others look
  like failures or forgeries (sign v4, [#555](https://github.com/people-post/pp-browser/issues/555); capability
  offers each side refuses; unparsable addresses).
- Practice already drifted: the relay serves apps older than `sign_version` until 2026-12-31 (pp-relay-apis
  `94c6865`), a bridge the policy says does not exist.

## Scope

The rule covers every format between separately released components:

| Pair | Formats |
|------|---------|
| app ↔ app | envelope, account card, control lane, contact cards and locators, group bundles |
| app / pp-node ↔ relay | signed relay calls (`sign_version`), the card at registration, responses and error codes, `client-compat` |
| app ↔ pp-node, node ↔ node | Amp wire, addresses (`/adp/N/`), LAN discovery, the channel-0 capability offer, mesh protocol ids, media frames, DHT and directory records |
| stored data | the relay's tables and files; each device's local stores; the node's local state |

The pp-cpp-* libraries have no range of their own: they are compiled into the products, and the products' ranges
cover what they put on the wire.

## Rules

### Change classes

| Class | What it is | What it takes |
|-------|------------|---------------|
| **Additive** | A new optional field, content type, control type, error code or protocol id that older readers can ignore | No version change. Readers ignore what they do not know |
| **Format change** | A change older readers cannot ignore | A new version, rolled out by the [range rule](#the-range-rule) |
| **Security** | An older version is unsafe to keep accepting | May shrink the range at once: the affected version is refused immediately, still with "update needed". Each one signed off by Hongwei and recorded as a decision with its reason |

### The range rule

A format change rolls out in three steps:

1. **Readers first.** A release reads the old and the new form. It ships before anything writes the new form.
2. **Then the writer switches.** A release writes the newest form that **the oldest version in range** can read.
   Senders do not need to know what each recipient reads: writing for the oldest supported version is enough.
3. **Then the old reader goes.** Once the last version that writes the old form has left the range, the reader of
   the old form is deleted. Old forms do not accumulate.

Steps 1 and 2 may be the same release when nothing in range writes the old form any more.

### Range length

A version stays in range at least as long as the longest of:

- the life of the release after it (a version leaves only after its successor has been out for a while);
- the life of stored data it writes (relay inbox retention, kept cards, chat history);
- a minimum time.

Ranges differ by pair, because phones update slowly (RC006):

| Pair | In range |
|------|----------|
| relay ↔ app | the last 3 app releases, and at least 90 days |
| app ↔ app, app ↔ pp-node, node ↔ node | the last 2 releases, and at least 60 days |

Days count from the release date. What share of users still run a version may inform when a range closes, but
does not decide it.

### Rollback

- **Relay:** must roll back one release without losing data. It never writes a record its previous release cannot
  read. A new record shape is written only after a release that reads it has been deployed.
  A rollback is a redeploy of the previous release tag ([§ Branches and tags](#branches-and-tags)).
- **pp-node:** should roll back one release the same way.
- **Apps:** local data moves forward only; downgrading an app is not supported (phones cannot
  downgrade from the store). A store newer
  than the build is refused with the start-up recovery, as today.
- **Stores only the same install reads are exempt from the range rule** ([RC015](DECISIONS.md#rc015--client-side-answers-after-kenneths-review)): `profile.db`, `thread.db`, local
  preferences and the other files of one device's profile migrate forward through their ladder
  ([DATA_LAYOUT § Schema versions](../../docs/contracts/DATA_LAYOUT.md#schema-versions)) and never carry readers for
  older releases' forms. **Data another install reads is not exempt:** the account backup, device names and what
  linked devices sync are read by other devices of the account, which may run other releases, so they follow the
  range rule like the wire.

### Versions are announced and checked

- Each format carries a version that the receiver checks **before** anything else, including before verifying a
  signature: a signed object carries its version both inside the signed bytes and readable outside them (the rule
  after #555, today only for envelopes).
- Each component announces its range where it already introduces itself:
  - the relay: `client-compat`, with its release version and supported ranges;
  - pp-node and apps on a link: the channel-0 capability offer (it already carries the wire version, node version
    and features, N032);
  - apps and nodes to the relay: `sign_version` on every signed call, as today.
- A range announced inside a signed object or a handshake is covered by its signature or transcript, so it cannot
  be stripped to force an older version.
- Every refusal caused by a version names who needs to update: the relay's `426` with `upgrade_urls`, an
  "update to view" placeholder for a message, a peer marked as needing an update. A version mismatch never looks
  like a forgery or a network failure.

### Readers tolerate what they do not know

- JSON and records: unknown keys are ignored everywhere.
- An unknown content type, a higher payload version or a higher `envelope_version` is skipped with "update to
  view", and its sequence number is still consumed so sync does not stall.
- Unknown control types, mesh protocol ids, capability-offer features and relay error codes are ignored, or read
  as a plain refusal with a reason. Never a stall or a loop.
- Crypto stays strict: a signature, AEAD or decryption failure is a hard refusal.

## The version table

One table in `docs/contracts/` (promoted in phase P4) lists every format: its current version, the oldest version
readers accept, its owner, and the release at which its oldest version leaves the range. It replaces the
"Clean cutovers" table in COMPATIBILITY.md, which stays as history ending with release 1.

App, pp-node and the relay each have their own version ([RC016](DECISIONS.md#rc016--app-pp-node-and-relay-each-have-their-own-version)), so the table also lists, for each release of each
product, the format versions it writes and reads. Compatibility is decided by those format versions; a product number
only names a release.

Starting inventory (from COMPATIBILITY.md and the 2026-11-17 review), checked in phase P0: [INVENTORY.md](INVENTORY.md)
has every row's version, its check and what an unknown version or field does today:

| Format | Version today | Owner |
|--------|---------------|-------|
| Envelope / sign | `envelope_version` 3, sign v5 | pp-browser (delivery) |
| Account card | 4 | pp-browser (people), pp-relay-apis |
| Relay grant | `pp-browser:relay-grant-v1` | pp-browser (delivery), pp-relay-apis |
| Contact card / locator | `sub_version` 3, `pp-browser-contact-v1:` | pp-browser (people) |
| Relay signed calls | registration v3, relay API v3, others v2 (`sign_version`) | pp-relay-apis |
| `client-compat` | `min_client_version` / `latest_client_version` (app only) | pp-relay-apis |
| Amp wire | 2 (version byte) | pp-cpp-amp |
| Addresses | `/adp/2/` | pp-cpp-amp |
| LAN discovery | mDNS TXT `wire=` | pp-cpp-amp |
| Channel-0 capability offer | leading byte `4` | pp-cpp-amp, pp-cpp-mesh |
| Media frames | 4 (extension block) | pp-cpp-mesh |
| Mesh protocols | version in each protocol id | pp-cpp-mesh, pp-browser |
| Control lane (per-device copies, card and copy asks) | to check in P0 | pp-browser (delivery) |
| Group chat messages and bundles (roster, group copies) | to check in P0 | pp-browser (messaging) |
| Attachment metadata (blob references, keys, sizes) | to check in P0 | pp-browser (delivery) |
| Account backup format (the account locker) | to check in P0 | pp-browser (people), pp-relay-apis (storage) |
| Support client contract (the Support account, `client-compat` `support`) | to check in P0 | pp-browser, pp-relay-apis |
| AI client contract (`/api/llm/v1`: `chat/completions`, `pp/chat/stream`, guest keys) | to check in P0 | pp-browser (ai), pp-relay-apis (llm-gateway) |
| Relay tables | no record versions; card version inside | pp-relay-apis |
| Local stores (exempt: one install reads them, [§ Rollback](#rollback)) | `profile.db` 8, `thread.db`, `contacts.json`, `identity.enc`, `our_relays.json`, `relay_pins.json`, preferences, … | pp-browser |

## Release 1

Release 1 is the floor of every range. It ships across app, pp-node and relay together, the last time anything
ships in lockstep. Phase P0 turned this list into exact work items: [INVENTORY § 4](INVENTORY.md#4-release-1-work-list).

**Needed, because the rule cannot hold without them:**

- Readers that tolerate what they do not know ([above](#readers-tolerate-what-they-do-not-know)). Today a higher
  `envelope_version` stalls the thread in Gap, an unknown content type cannot be stored and stalls the same way,
  and a higher payload version reads as "couldn't decrypt" ([INVENTORY § 2.1](INVENTORY.md#21-app--app)).
- A version every receiver checks first, for every signed object, not only envelopes.
- A clean "update needed" for every version refusal. Its screen's text and translations are Kenneth's; prod's update
  links (`upgrade_url`, `upgrade_urls`) are Leo's (P4).
- The relay with release versions it reports, and its range in `client-compat`; a node floor beside the app's.
- A clean start: the relay's legacy window for older apps (pp-relay-apis `94c6865`) removed, pre-release readers
  and local-store migration steps below release 1 dropped (the floors move to release 1), and `min_client_version`
  raised to release 1.

**Worth doing now, because they are cheapest before anything older exists** (each could also come later through
the range rule):

- A signed extension area in each signed object (envelope, card, relay calls, grants): covered by the signature,
  unknown entries ignored. Today unknown envelope keys are not signed, so each new signed field is a format change.
- An extension block in each binary format that lacks one (media frames have one).
- Every key, signature, KEM and AEAD blob names its algorithm, so a later algorithm is a new version, not a new
  identity.

**Also needed, found in the 2026-11-18 review** ([RC011](DECISIONS.md#rc011--operating-a-live-release-what-release-1-also-needs)):

- **Versions in use are counted.** The relay counts requests per app and node release version per day, as aggregates
  only, never per account (the release version in a request header; `sign_version` it sees already). Range exits,
  canary judgement and floor raises read these counts.
- **Remote switches.** `client-compat` carries a short list of named switches (a feature or a call path off) that
  apps read at start-up, so a broken feature can be turned off in versions already on phones. The reader ships in
  release 1: a version without it can never be switched.
- **App ↔ relay behaviour tests.** The app's expectations of each relay route (what it accepts, returns, matches and
  refuses) are a small suite run against the relay's local stack in both repos' CI. Frozen data checks formats, not
  behaviour: it would not have caught the search 400 of 2026-11-17.
- **Relay identity rotation and compromise recovery.** Clients pin each relay's PeerId (MR008), so today a new key
  strands every client and a leaked key has no recovery. Decided ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) item 7): as in pp-ledger, an offline
  operator set certifies the relay's transport and signing keys, and clients pin the operator key, not the PeerId.
  It ships in release 1, because in-range apps must understand it.
- **Account deletion, finished.** App Store guideline 5.1.1(v) requires an app that creates accounts to let users
  delete them. It exists as Me → Storage → *Reset my data…* (`SettingsSecurityActions::ResetProfile` →
  `Application::ResetActiveProfile`): it revokes the registration on every relay of ours (others first, then the
  preferred; best effort, so an offline reset still goes ahead), the relay removes the account's rows and purges its
  push devices, and the device is wiped. Left for release 1:
  - **Say it deletes the account.** The confirmation reads "Deletes chats, contacts and the account's encryption data
    on this device", which names neither the account nor the relay; users and App Review must be able to find
    account deletion as such.
  - **What the relay keeps after a revoke.** `SPECS.md` says a revoke removes the registration and purges push
    devices; it does not say what happens to the account's mailbox (up to 30 days), public icon, private files,
    account lockers, Support conversation or usage records. Each is deleted on revoke, or its retention is stated.
  - **An offline reset** leaves the registration until it expires (up to 90 days): retry the revoke later, or say so.
  - **Several devices of one account:** whether resetting one device revokes the account's registration for all of
    them (registration is one per Account ID), and whether that is the intent. Decided: two actions, *Delete account…* and
    *Remove this device…* ([RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) item 1).
- **Crash reports from released versions.** The app sends them when `client-compat` names a `crash_reports_url`;
  prod names none. Release 1 needs a destination, within DIAGNOSTICS' shareable-lines rule.

**Also:** breaking work already planned or half done. P0's inventory decides which of it lands before the freeze;
the rest waits and follows the range rule.

## Tests

- **Unit tests read frozen data** of the oldest supported version of every format (release 1 at first). This
  extends the relay sign vectors (`relay-sign-vectors.json`) and the envelope sign-bytes hash
  (`EnvelopeSignerTest.SignBytesMatchFrozenHash`). A change to signed bytes without a version bump fails CI.
- **A lab run mixes versions:** the oldest and newest supported app, pp-node and relay, built from released
  artifacts, on the scenarios that cross the changed format.
- Each PR that changes a format says, in the template's *Contracts and docs*: the format, its class, the versions
  on each side, when the old one leaves the range, and the rollback.

## Branches and tags

Under the rule, several releases are live at once, and each one still in range may need a fix (a security fix, a
crash, a relay bug that hits old apps). Today's develop → main flow has no home for a fix to release N once main
has moved on: main is develop running behind, and shipping a fix means shipping everything since N, format changes
included. Internal builds leave no tag either (on 2026-11-17, "TestFlight 136" had to be found by matching workflow
run numbers plus 100). Leo's review of the first draft (#634) added the tag, environment, build-once and check-level
rules below; [RC007](DECISIONS.md#rc007--the-branch-model-after-leos-review) records what was taken from it and why
the rest was not.

### Release lines are first-class

The product does not have one production. Apps update when their owners update them, pp-node runs on users'
machines, and under multi-relay relays may be run by others; so "production" is every version still in the
supported range (RC006: the last 3 app releases and at least 90 days), and each of them is a release line that may
need a fix. Branches follow from that:

**One integration branch, `main`.** All feature PRs go to `main`; CI must pass to merge. `develop` is retired in
every repo. (The name is not the point; having one long-lived branch is.) **`main` is the latest integration
baseline, not production:** what runs in the field is the immutable release tags and the live release branches.
Wherever "main" could be read as "what is deployed", it is not. **Every PR into main needs one approval from someone
other than its author** ([RC015](DECISIONS.md#rc015--client-side-answers-after-kenneths-review)), enforced by branch protection, with the code owners of its paths
(`.github/CODEOWNERS`); release branches take the release owner's approval (RC014).

**One branch per release, cut from main**, for every release that ships into the field, named by its product
([RC016](DECISIONS.md#rc016--app-pp-node-and-relay-each-have-their-own-version)): `release/app-X.Y` and `release/node-X.Y` in pp-browser, `release/X.Y` in pp-relay-apis. Below,
`release/X.Y` stands for whichever product's branch is meant.

- Release tags (`vX.Y.0`, `vX.Y.1`, …) go on that branch. They are immutable, and release workflows accept no others.
- A release branch takes **fixes only**: no features and no format changes. Additive changes only where a fix needs
  them; a security cutover ([§ Change classes](#change-classes)) may go onto a branch.
- A release branch stays open while its release is in range and closes when the release leaves it: **the live
  release branches are the supported range**. The newest release branch takes every fix; older branches still in
  range take only security fixes and fixes for a break between in-range versions.
- **Closing a branch:** the release owner closes it when the version table says its release has left the range.
  Open backport PRs to it are closed with a note naming the range exit; the branch is then protected against pushes
  but kept, because its tags and history must stay.
- The relay has one prod deployment, but its release branch matters as much: main may already hold the reader half
  of a format change ([§ The range rule](#the-range-rule)) that prod must not get with a hotfix.

**Fixes land on main first, then are backported** to each live release branch that needs them. Release branches
are never merged back into main: main already has every fix. The backport rules
([RC014](DECISIONS.md#rc014--backport-and-hotfix-rules-after-leos-second-review)):

1. **A label opens a PR to that exact branch.** `backport release/app-1.1` on a merged main PR makes a bot open a PR
   targeting `release/app-1.1`. A fix in code both products share takes a label per product branch that needs it. The bot does the cherry-pick mechanically; the backport PR runs that branch's own
   checks and still needs a human approval (that release's owner may give it).
2. **Provenance is kept.** Each backported commit keeps its `(cherry picked from commit …)` trailer, and the PR links
   the original main PR, so which fixes reached which release lines can be audited.
3. **Conflicts are reported, not adapted silently.** When the cherry-pick does not apply cleanly, the bot opens a
   draft PR (or comments on the original PR) naming the conflict. An adaptation is a separate commit marked
   "branch-specific backport", explaining what differs.
4. **A fix only for a release branch is an exception.** It carries an exception label and says why main needs no
   equivalent change (for example the code is gone from main).

**A hotfix starts from the affected release branch**, not only from the newest release: a fix for app 1.1 while 1.3 is
current is made on `release/app-1.1` (by backport, or by the exception above), runs that line's checks, gets a new
patch tag on that branch (`v1.1.Z+1`, its candidates `-rc.N` first), and follows the same build-once promotion as any
release ([§ App builds](#app-builds-internal-external-store)). That is what makes the range promise real for older
app versions.

### Builds, tags and deploys

**Every internal build is tagged** by the workflow that makes it, so every build anyone ran maps to one commit:

| Build | From | Tag |
|-------|------|-----|
| App dev builds people install (TestFlight, Android, desktop test builds) | any branch or tag, by hand | `internal/<build number>` |
| App release candidates | `release/app-X.Y` | `vX.Y.Z-rc.<n>` |
| pp-node release candidates | `release/node-X.Y` | `pp-node/vX.Y.Z-rc.<n>` |
| Internal pp-node builds | main | `node-internal/<date>.<n>` |
| Relay dev deploys | a commit on main, tagged by hand | `dev/<date>.<n>` (with Leo; [RC021](DECISIONS.md#rc021--the-relay-repos-deploy-by-tag-only)) |
| Relay release candidates | `release/X.Y` | `release/vX.Y.Z-rc.<n>` |

CI builds nobody installs, and builds a developer installs on their own device (USB, a local run), get no tag: tags
are for builds someone else installs.

**Build once, promote the same artifact.** A release candidate is built once from its tag; the artifact that passed
is the one released (`commit → build once → test the artifact → staging / canary → production`). This needs
artifacts that do not depend on their environment: the relay takes its settings from the deploy, and an app's
release version is set before its candidate build, so the TestFlight build that passed is the one sent to the store.

**Deploys are gated by GitHub environments:**

- the relay repos deploy by tag only, never by branch push ([RC021](DECISIONS.md#rc021--the-relay-repos-deploy-by-tag-only)):
  a `dev/<date>.<n>` tag on a commit on main deploys dev;
- a `release/vX.Y.Z-rc.<n>` tag on `release/X.Y` deploys staging (today's qa) ([§ Environments](#environments)); the
  prefix `release/` with a `v...-rc.<n>` tail never equals the branch name `release/X.Y` and does not match `v*`;
- only `v*` tags on `release/X.Y` deploy to production, behind an approval gate; for apps, only tags go to the stores;
- rollback redeploys the previous release tag's artifact (RC003).

The relay's API repository and the relay node fleet's deploy repository (the playbooks) follow this scheme, with the
same tags; pp-node fleet rollouts remain Leo's. The relay reports its
version as `dev.<date>.<n>` (dev), `X.Y.Z-rc.<n>` (staging) and `X.Y.Z` (prod). qa is retired from these repos' deploy
flow; dev, staging and prod are the live relay environments. App and pp-node candidate tags are unchanged.

### App builds: internal, external, store

An app build moves along one ladder, and the same uploaded binary climbs it: App Store Connect gives one upload to
internal testers, then external testers, then the App Store, with no rebuild (Google Play's internal → closed →
production tracks promote one app bundle the same way; desktop releases promote the candidate's artifacts). The
rules ([RC008](DECISIONS.md#rc008--app-builds-climb-one-ladder-only-release-candidates-are-promoted)):

| Build | Made from | Goes to | Tag |
|-------|-----------|---------|-----|
| **Dev build** | a manual trigger, any branch or tag (latest features, a fix to try) | internal testers only; **never promoted** | `internal/<build number>` |
| **Release candidate** | pushing `vX.Y.Z-rc.<n>` on `release/app-X.Y`; built and uploaded to internal testers automatically | internal, then external testers, then the store | `vX.Y.Z-rc.<n>`; `vX.Y.Z` on the same commit when it is submitted to the store |

- **Only release candidates are promoted.** The stores see a version string and a build number only, so a dev build
  stamped with the next version looks like a candidate. A candidate is built with its final version (1.1.0); only
  build numbers differ between candidates.
- **Promotion is a workflow, not clicks.** A workflow adds a candidate to the external testers, then submits it for
  review, through the App Store Connect API (and the Play Developer API). It refuses a build whose tag is not a
  release candidate, runs behind a GitHub environment approval, and tags the release (`vX.Y.Z`) on the candidate's
  commit when it submits.
- **Store builds know prod only** ([RC015](DECISIONS.md#rc015--client-side-answers-after-kenneths-review)). Release candidates, and so the store builds they become, compile in the
  prod relay alone; Me → Network offers prod and a custom server, as Leo asked, and no Staging row reaches store
  users. Testers reach staging on a candidate through a custom relay link (`https://…#<PeerId>`), which release
  builds accept when the link names the PeerId ([CONFIGURATION](../../docs/ops/CONFIGURATION.md)); the binary they
  tested is the one prod users get. Dev (tester) builds compile in staging and dev, never prod, and switch in
  Me → Network (`backend.env`): they share the store app's data container, so they never open a production profile
  ([RC022](DECISIONS.md#rc022--tester-builds-never-touch-production-profiles-hard-guard)).
- **Hotfixes** take the same path from their release branch. A security fix may go from internal testers straight
  to the store with an expedited review, skipping external testers.
- **Store releases are phased.** The App Store's 7-day phased release (and a staged rollout on Play) is used for
  every release, and paused when something goes wrong: a bad version stopped at a few percent is far cheaper than
  one on every phone, since it cannot be taken back.
- **Review times are planned for.** The first build of each version needs Beta App Review before external testers
  get it (usually under a day); later builds of the same version usually do not. The first candidate is cut early.
- **A build says what it is.** The app shows its tag and commit in About and in its diagnostics, so a report names
  the build it came from.
- **Later, after release 1: a separate dev app** ("PP Dev", its own bundle id), so a tester can keep the store
  version and a dev build on one phone for mixed-version testing. It needs a second App Store Connect record, its
  own push and VoIP certificates, app group and keychain.

### Checks by level

| Level | Checks |
|-------|--------|
| Feature PR to main | Build, unit tests, static checks, focused smoke tests (as today) |
| Release candidate on `release/X.Y` | Full CI on every shipped platform, the frozen-data compatibility tests, the mixed-version lab run, the release build |
| Fix or backport to a release branch | All unit tests, the frozen-data compatibility tests, every platform build that ships, the lab scenarios the fix touches (`--only`); for the relay, its integration tests and the local-stack end-to-end test |
| Version tag | No new build: deploy the tested artifact to staging / canary, then production after approval |

The nightly lab and the full lab run on main and before each `.0` release; a fix does not wait for them.

### Libraries and numbering

**Libraries stay simpler.** pp-cpp-* keep main plus version tags. A library release branch is cut only when a
product's release branch needs a library fix without taking a newer major version.

**App, pp-node and the relay each have their own version** ([RC016](DECISIONS.md#rc016--app-pp-node-and-relay-each-have-their-own-version)), as the repo's two trains already do (`v*` for
the app, `pp-node/v*` for the node, `docs/ops/RELEASE.md`):

| Product | Tags | Release branch | Release 1 |
|---------|------|----------------|-----------|
| App | `vX.Y.Z` (pp-browser) | `release/app-X.Y` | **1.0** (its first public release; 1.0.0 today) |
| pp-node | `pp-node/vX.Y.Z` (pp-browser) | `release/node-X.Y` | **10.0** (the next major after v9) |
| Relay | `vX.Y.Z` (pp-relay-apis) | `release/X.Y` | **1.0** |

Each ships on its own cadence: an app release waits on store review, a node release goes to the fleet and user-run
nodes, a relay release deploys; a node-only fix needs no app release. What works with what is the
[version table](#the-version-table)'s map of product releases to the format versions they speak, not a shared
number. "Release 1" names the floor across all three.

## Environments

Before release mode the relay had two environments: prod, and qa, which took every develop merge and was also the
only place to test before prod. The plan needs three jobs kept apart: integration (the latest main, which may
break), release candidates (the exact artifact to be promoted, tested in prod-like conditions), and production
(rolled out gradually, because a relay deploy reaches every app version at once). Today's qa becomes **staging**,
and **dev** is new ([RC009](DECISIONS.md#rc009--four-environments-todays-qa-becomes-staging-dev-is-new)):

| Environment | What deploys there | Who uses it | Data |
|-------------|--------------------|-------------|------|
| **local** (local-stack and the lab, as today) | any branch, in CI and lab runs | developers, CI | thrown away after each run |
| **dev** (new) | a `dev/<date>.<n>` tag on a commit on main, pushed by Leo | developers, dev builds | may be reset |
| **staging** (today's qa) | release candidates tagged `release/vX.Y.Z-rc.<n>` on `release/X.Y`; the previous release for rollback rehearsals | release testing, internal TestFlight candidates | prod-like and kept, no real users |
| **prod** | `v*` tags on `release/X.Y` only, behind an approval, canary first | users and external testers | real |

**Staging keeps everything qa has today**, so nothing on the wire or on disk changes: dev builds compile in its
origin (`PP_API_URL_QA`), users' `config.json` keeps `backend.env: "qa"`, and the relay keeps its PeerId
(`PP_RELAY_PEER_ID_QA`). In dev builds the label in Me → Network becomes "Staging"; store builds do not compile it
in, and a `backend.env: "qa"` there loads as prod, as it already does for a build without a QA origin ([RC015](DECISIONS.md#rc015--client-side-answers-after-kenneths-review)).
What changes is what deploys there: tagged release candidates, no longer every merge.

**Staging is where a candidate is proven:**

- the exact artifact that will be promoted (RC007's build once), with prod-like settings;
- mixed versions: the store app (N−1) and the candidate (N) against the staging relay, beyond the lab;
- a rollback rehearsal: the previous relay release deployed onto data the candidate wrote, proving RC003 before prod
  relies on it;
- migrations and settings: the `client-compat` floor, update links, a window closing.

**Dev is reached without a new build**, through Me → Network's custom relay link (`https://…#<PeerId>`), which
release builds accept when the link names the PeerId. Compiling it in as a third origin can come later if testers
use it often.

**Prod rolls out gradually.** A relay release goes to a share of prod traffic first (Lambda weighted aliases), is
watched, and is rolled back automatically when its alarms fire, the server's version of a phased app release. RC003
makes this safe: while both versions run, each reads what the other writes.

**External testers stay on prod.** They run the candidate app against the current prod relay, the real "new app,
current relay" pair. The relay still ships first: staging, then the prod canary, then the app candidate to external
testers.

**Each environment is separate:** its own tables, buckets, secrets, relay identity (PeerId), LLM upstream token and
pp-node seeds. pp-node gets matching tiers in the ansible inventories (dev, a small staging node or two, prod). Prod
moves to its own AWS account, apart from dev and staging (decision below).

**Cost:** the relay is serverless (Lambda, on-demand DynamoDB), so an idle environment costs almost nothing; the cost
is the staging pp-node machines and the work in terraform, ansible and the workflows.

## Backups and restore

Recommended to Leo for review ([RC010](DECISIONS.md#rc010--backups-continuous-on-every-release-and-rollback-copied-out-of-prod)).
What the relay keeps (pp-relay-apis main, 2026-11-18): tables `accounts`, `nodes`, `devices`, `blobs`, `messages`,
`usage`, `limits`, `admin`, `support`, `support-crypto`; buckets for account lockers, blobs and icons, files, keys and
diagnostics; secrets: the relay identity key, the Support identity seed, the LLM token signing key and upstream
token, the APNs and FCM credentials. Its `docs/architecture.md` says only `accounts` and `blobs` have point-in-time
recovery (`messages`, `limits` and `usage` do not); the others are not mentioned. The real settings are in the
infrastructure.

**Continuous backups, not schedules.** DynamoDB point-in-time recovery restores any second of the last 35 days. It is
on for every table that holds state: `accounts`, `nodes`, `devices`, `blobs`, `admin`, `support`, `support-crypto`,
and `messages` (mail not yet collected: encrypted, kept up to 30 days; losing it loses users' messages silently).
Not `limits` (short-lived counters); `usage` is optional (statistics). The buckets are versioned.

**Backups on events, taken by the workflows,** each tagged with the relay version that wrote the data:

- before each prod release (the deploy workflow);
- before a rollback, keeping what the bad release wrote, to investigate it and re-apply what was good;
- before any data change outside a release (a backfill, a cleanup, closing a compatibility window).

**A copy outside the prod account.** AWS Backup copies daily backups into a separate account (the backup or non-prod
account, decision 10) whose vault is locked: no one in the prod account can delete them, neither a leaked credential
nor a mistake. Dailies are kept 90 days; event backups as long as their release is in range. Retention is bounded on
purpose (privacy, below).

**Secrets are their own class.** Losing the relay identity key means a new PeerId: every app, card and relay link
pins it (MR008), so every client would refuse the relay with no way back. Losing the Support seed loses the Support
account. These secrets are replicated to a second AWS region and kept in an offline encrypted copy held by two
people; both copies are checked whenever a key rotates.

**Restores are practised.** A restore makes a new table: a runbook says how the functions move onto it (their
`TABLE_*` settings) or how the data is copied back. It is rehearsed every quarter on staging, beside the rollback
rehearsal. Targets: at most 5 minutes of data lost, service back within 2 hours.

**Rules for a restore:**

- backups hold sensitive data (support chats in plaintext, staff data, account metadata): encrypted with their own
  key in the backup account, and the privacy policy says how long deleted data lives on in them;
- a restore must not bring back deleted accounts: removals and revocations go to a short log, re-applied after any
  restore;
- a restore rolls back data, not code: data from before release N+1 restored under N+1 is covered by the range rule,
  older code on newer data by RC003; the runbook says so.

**By environment:** prod has all of the above; staging has point-in-time recovery (restore rehearsals need it); dev
has none.

## Release checklist

**The release owner for release 1 (app 1.0, pp-node 10.0, relay 1.0) is Hongwei** (RC015); the role rotates after it.

Today's list (`docs/ops/RELEASE.md` § Checklist before tagging) predates release mode. This one replaces it when
release mode ships (P4 moves it into `RELEASE.md`; until then it lives here). Each release gets its own copy: when
a product's release branch is cut, a workflow opens an issue for it ("Release app 1.0", "Release pp-node 10.0") from a template (`.github/ISSUE_TEMPLATE/release.md`)
and assigns the release owner, who ticks the boxes, links results and records the go/no-go there. What CI and the
workflows already check (tests, frozen data, the mixed-version lab, the staging deploy) is linked, not re-checked
by hand. The relay keeps its own list in pp-relay-apis (Leo's), linked from the issue.

**Before cutting `release/X.Y`**

- [ ] Format changes since the last release listed, each with its class and range-rule step; the version table updated
- [ ] Any relay release this one needs: deployed to prod first
- [ ] Floor raises or windows closing in this release: announced weeks earlier (the update banner)
- [ ] Freeze announced; release owner named

**Release candidate**

- [ ] `vX.Y.Z-rc.N` tagged; artifacts built once (iOS, Android, desktop, pp-node image)
- [ ] Full CI on every shipped platform, frozen-data tests and the app ↔ relay behaviour suite green
- [ ] Staging: the relay candidate deployed; the store app (N−1) and the candidate tested against it; relay rollback
  rehearsed
- [ ] Lab scenarios the release touches, and the latest nightly lab, green
- [ ] On real devices from internal testing: register, message, call, push and VoIP wake, link a device, account
  deletion
- [ ] Release notes and TestFlight "What to Test" written

**Go / no-go (release owner)**

- [ ] No open blocker; known issues listed
- [ ] Version counts reviewed: who is on what, and who an exit affects
- [ ] Rollback path written: the relay's previous tag, the switches that turn this release's features off
- [ ] Relay backup taken before the release

**Rollout**

- [ ] Relay: canary 10% for 30 minutes, alarms quiet, then everyone
- [ ] App: external testers, then the store through the promotion workflow (tags `vX.Y.Z`), phased release; crash
  reports and error-code alarms watched
- [ ] pp-node: image published; fleet rolled out (ansible, Leo); owners of user-run nodes told

**After**

- [ ] `client-compat`'s `latest_client_version` raised; the next floor raise scheduled
- [ ] The release branch's open period and the version table updated
- [ ] A postmortem if anything went wrong, its finding turned into a cheaper test

## Runbooks

The release is the most common action, not the only one; the rare, stressful ones need written steps most
([RC012](DECISIONS.md#rc012--a-runbook-for-every-operational-action-each-use-recorded)).

| Action | When | Who decides | Lives in | Before release 1 |
|--------|------|-------------|----------|------------------|
| **Release** | every release ([§ Release checklist](#release-checklist)) | release owner | pp-browser | yes |
| **Hotfix / backport** | a fix on an affected release branch (any in range, not only the newest): the release list, shortened; the backport rules of [§ Release lines](#release-lines-are-first-class) | release owner | pp-browser | yes |
| **Relay rollback** | a bad relay release in prod | anyone on call; the owner is told after | pp-relay-apis | yes |
| **App incident** | a bad app version on phones: pause the phased release, flip a remote switch, ship a hotfix; a store rejecting or delaying a fix mid-rollout (switches, relay-side mitigation, expedited review or an appeal; the relay never needs the new app, so a delay breaks nothing) | release owner | pp-browser | yes |
| **Restore from backup** | data lost or corrupted ([§ Backups and restore](#backups-and-restore)) | Hongwei and Leo | pp-relay-apis | yes |
| **Key compromise** | a leaked relay identity key, Support seed, LLM signing key or push credential | Hongwei and Leo | pp-relay-apis | yes |
| **Range exit** | raising `min_client_version` or the node floor; closing a compatibility window. A release that raises the minimum iOS or Android version strands users who cannot update: before their last supported release leaves the range, the version counts by OS are checked and that release kept longer or its users warned | release owner | pp-browser | before the first window closes |
| **Security release** | a vulnerability: every in-range branch fixed, embargoed until all publish together | Hongwei | pp-browser | before the first window closes |
| **Key rotation** | scheduled, not an emergency | Leo | pp-relay-apis | later |
| **pp-node fleet rollback** | a bad node release (ansible) | Leo | the relay's infrastructure | later |
| **Environment resets** | dev at will, staging rarely | Leo | pp-relay-apis | later |

**Each runbook has the same shape:**

1. **When to use it**, and when not to (for example: roll the relay back only to a release that reads the data the
   current one wrote, which RC003 guarantees for one release).
2. **Who may decide**, and who is told.
3. **Before acting:** for example the pre-rollback backup (RC010), and which version counts are affected.
4. **Steps**, each one a workflow run where possible (a manual trigger with inputs, behind an approval), not console
   clicks: the runbook stays short and every action leaves a record.
5. **Verify:** what shows it worked (alarms quiet, the smoke run passing, `client-compat` answering the expected floor).
6. **Communicate:** users (the `client-compat` message, store notes), testers, the team.
7. **Follow up:** the incident issue, a postmortem, the cheaper test that would have caught it.

**Where they live:** `docs/ops/runbooks/`, one short file per action, a page each; the product's in pp-browser, the
relay's in pp-relay-apis, each linking the other's. **Each use leaves a record:** the release issue for releases, an
*incident* issue template (holding the runbook's checklist) for rollbacks, restores and compromises, ticked while the
steps are done. **They are rehearsed:** rollback and restore quarterly on staging (RC010), and key-compromise recovery
in the same rehearsal once identity rotation exists (RC011). A runbook never run is a guess.

## Regression suites

The lab is too long for a developer to run whole (about 20 suites in `scripts/test/pp_local_test.sh`; the app lab
alone, `scripts/test/pp_app_lab.py`, has 103 scenarios), and nothing ran it on a schedule: on 2026-11-18 the last 24
nightly runs (`.github/workflows/nightly-lab.yml`) were all skipped, because the repo variables `PP_LAB_INSTANCE_ID`
and `PP_LAB_AWS_ROLE` that start the lab machine were never set. Tiers, each with a time budget
([RC013](DECISIONS.md#rc013--regression-suites-in-tiers-picked-by-capability-run-remotely)):

| Tier | What runs | When | Budget |
|------|-----------|------|--------|
| **PR** | unit and capability-scoped tests (as today) | every PR | ~5 min |
| **Lab smoke** | ~10 scenarios over the core flows: register, first contact, message, find someone, call, push and VoIP wake, link a device, account deletion | every merge to main; on demand | ≤15 min |
| **Affected** | the scenarios tagged with the capabilities a change touches | on demand, any branch | ≤30 min |
| **Nightly full** | every suite on main (app, app-nat, hard, …); findings filed as issues | nightly | ≤2 h |
| **Release** | the full run plus mixed versions (the store app N−1 against the candidate) | each release candidate ([§ Release checklist](#release-checklist)) | as needed |
| **After a deploy** | the scripted smoke run against the environment just deployed (dev, staging, prod during the canary) | every relay deploy | ≤5 min |

- **Scenarios are tagged by capability** (messaging, calls, groups, relay, push, mesh, …). `--tags calls` runs every
  calls scenario; `--changed` works the tags out from the branch's diff, with the capability paths CI already uses to
  skip tests. "Run what my change touches" needs no scenario names.
- **Developers run the lab remotely**: the manual trigger takes tags or `--changed`, runs on a CodeBuild runner, and
  returns an artifact and a summary. Runs are independent (one runner each).
- **The full run gets shorter, not only rarer:** per-scenario times kept from `--summary` and the slowest fixed first;
  the run split across two or three runners; a tier over its budget is a finding like a failing test.
- **Flaky scenarios are named:** one that passes only on retry (`--retry-failed`) is recorded as flaky; if it stays so,
  it leaves smoke and the PR-facing tiers for a quarantine with an issue. A lab failure a unit test can reproduce gets
  that test ([TESTING § When a higher tier finds a bug](../../docs/architecture/TESTING.md#when-a-higher-tier-finds-a-bug)).
- **The lab runner is set up**: a privileged CodeBuild runner project in pp-runners-infra plus the relay-checkout
  secret. The cheapest high-value item here.

The tiers move into `docs/ops/TEST_STRATEGY.md` (the home of test tiers) when release mode ships.

## Not yet designed

Found in the 2026-11-18 review. Each needs its own design before it is relied on; until then it is tracked here.

| Topic | What is missing | Owner |
|-------|-----------------|-------|
| **Release cadence, owner, incidents** | How often releases ship (with weekly releases "the last 3" is 3 weeks, so the 90-day minimum is what holds); one owner per release (the go/no-go is in [§ Release checklist](#release-checklist)); who may pause a phased release or roll the relay back; how users are told; a short postmortem per incident whose finding becomes a cheaper test | Hongwei |
| **Notice before a floor raise** | The "update soon" banner (`latest_client_version`, `message`) for some weeks before `min_client_version` rises, so no one meets a blocking update screen unwarned | Hongwei, Leo |
| **Security process** | A `SECURITY.md` (how to report a vulnerability); a security release (every in-range release branch fixed and published together); secret rotation on a schedule | Hongwei, Leo |
| **Relays and nodes run by others** | The app's minimum relay version (the mirror of `min_client_version`, so it can say a relay is too old); how owners of user-run nodes learn an update is due | Hongwei, Leo |
| **Libraries change the wire too** | pp-cpp-amp and pp-cpp-mesh (and the others that put bytes on the wire) follow the range rule: the old form stays readable for its window, with their own frozen-data tests | Hongwei |

## Decisions taken

Hongwei, 2026-11-17 (RC006). Items 3 and 8 touch the relay's workflows and wait for Leo.

1. **Range length:** by pair, from the release date ([§ Range length](#range-length)): relay ↔ app the last 3 app
   releases and at least 90 days; app ↔ app and with pp-node the last 2 releases and at least 60 days.
2. **Security cutovers:** each signed off by Hongwei, recorded as a decision with its reason.
3. **Relay versions:** its own semver, `vX.Y.Z` on `release/X.Y`, reported with its ranges in `client-compat`.
   *(Leo)*
4. **App downgrade:** out of scope; local data moves forward only.
5. **Breaking work for release 1:** decided by P0's inventory; whatever misses the freeze follows the range rule.
6. **The version table:** one table in pp-browser's `docs/contracts/`; pp-relay-apis `SPECS.md` links to it and
   lists only the relay's own formats.
7. **One number for app and pp-node:** no, superseded by [RC016](DECISIONS.md#rc016--app-pp-node-and-relay-each-have-their-own-version) (2026-11-19): each product has its own version;
   release 1 is app 1.0, pp-node 10.0, relay 1.0.
8. **Internal build tags:** `internal/<build number>` (apps), `node-internal/<date>.<n>` (pp-node),
   `dev/<date>.<n>` (relay dev deploys; `qa/` before RC009); none for CI builds nobody installs. *(Leo, for the
   relay's)*
9. **Fixes on release branches:** every fix on the newest; security fixes and fixes for a break between in-range
   versions on older ones in range.

From [§ Environments](#environments) (RC009): agreed by Hongwei on 2026-11-18, **recommended to Leo for review**
(his infrastructure):

10. **Prod's AWS account: prod stays; dev and staging move out.** Moving prod later means migrating live tables,
    buckets, the CDN distribution, secrets and the relay's identity key with users on it. If prod and qa share an
    account today, that account becomes prod-only and a new non-prod account holds dev (built there from the start)
    and staging (moved at the switch, keeping its domain by DNS and its identity key by copying the secret, so apps
    notice nothing). If they are already apart, dev joins the non-prod one.
11. **The prod canary: 10% for 30 minutes, then all** (CodeDeploy's `LambdaCanary10Percent30Minutes`). It rolls back
    on Lambda errors and throttles, the 5xx rate, p99 latency, and a jump in **client-visible error codes** (400,
    401 such as `wrong_audience` or signature failures, 426, 404 `not_registered`): a contract break like the search
    400 of 2026-11-17 shows there, not as a crash. While traffic is low, a scripted smoke run covers the window too:
    relay identity, `client-compat`, lookup, register, send and poll.
12. **Dev's data may be reset at any time**, by a workflow that posts a note when it runs.
13. **Staging starts fresh once at the switch** (best together with its move in item 10), keeping its domain and
    identity: qa's data comes from the pre-release cutovers and is outside every range. From then on it is kept,
    because rollback rehearsals need data a real release wrote. P2 checks that apps re-register by themselves when
    staging answers `not_registered` (not yet verified).
